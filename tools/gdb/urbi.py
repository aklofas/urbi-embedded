# tools/gdb/urbi.py - GDB Python pretty-printers + walkers for urbi-embedded.
#
# Load:  gdb -x tools/gdb/urbi.py PROGRAM
#   or:  (gdb) source tools/gdb/urbi.py
# Needs a build with debug info (-g). Reads live OR core-dump target memory, so
# the walkers work on a halted/wedged target (the emergency-dump use case).
#
# Commands: urbi-strands [VM], urbi-handles [VM], urbi-heap [VM],
#           urbi-allocs [VM] [N], urbi-leaks [VM] (URBI_MEM_DEBUG),
#           urbi-trace [VM] [N], urbi-dump [VM].   VM defaults to `vm` in scope.
#
# The CHANNELS/SCHEMAS tables mirror src/runtime/utrace_format.c - keep in sync.
import gdb
import gdb.printing

CHANNELS = ["vm", "sched", "gc", "watcher", "event", "tag", "repl", "user"]
SCHEMAS = [
    "milestone", "sched_create", "sched_start", "sched_block", "sched_yield",
    "sched_resume", "sched_exit", "gc_phase", "gc_slice", "gc_alloc_denied",
    "watcher_install", "watcher_fire", "watcher_complete", "event_emit",
    "event_drain", "tag_op", "repl_session", "repl_eval", "user_marker",
]
LEVELS = ["DEBUG", "INFO", "WARN", "ERROR"]
STATE_CLASS = {0x00: "DORMANT", 0x10: "READY", 0x20: "RUNNING",
               0x30: "WAITING", 0x40: "DEAD", 0x50: "SUSPENDED"}
STATE_REASON = {0x00: "-", 0x01: "SLEEP", 0x02: "WATCHER", 0x03: "EVENT",
                0x04: "JOIN", 0x05: "HOST", 0x06: "BLOCK", 0x07: "FREEZE"}
VAL_KIND = {0: "nil", 1: "int", 2: "float", 3: "bool", 4: "str", 5: "closure",
            6: "void", 7: "strand", 8: "object", 9: "event", 10: "host_fn", 12: "tag"}


def _name(table, i):
    return table[i] if 0 <= i < len(table) else "?%d" % i


def strand_state_str(state):
    cls = state & 0xF0
    reason = state & 0x0F
    s = STATE_CLASS.get(cls, "0x%02x" % cls)
    if cls in (0x30, 0x50):  # WAITING / SUSPENDED carry a reason sub-code
        s += ":" + STATE_REASON.get(reason, "0x%x" % reason)
    return s


# ---- pretty-printers -------------------------------------------------------

class UValuePrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        kind = int(self.val["kind"])
        name = VAL_KIND.get(kind, "kind%d" % kind)
        v = self.val["v"]
        if kind == 0:
            return "nil"
        if kind == 1:
            return "int %d" % int(v["i"])
        if kind == 2:
            return "float %g" % float(v["f"])
        if kind == 3:
            return "bool %s" % ("true" if int(v["i"]) else "false")
        return "<%s @ 0x%x>" % (name, int(v["p"]))


class UStrandPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        addr = int(self.val.address) if self.val.address else 0
        return "UStrand id=%d state=%s @0x%x" % (
            addr & 0xFFFF, strand_state_str(int(self.val["state"]) & 0xFF), addr)


class UTraceRecordPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        r = self.val
        ch = _name(CHANNELS, int(r["channel"]))
        sch = _name(SCHEMAS, int(r["schema_id"]))
        lvl = _name(LEVELS, int(r["level"]))
        return "seq=%d t=%d strand=%d %s/%s %s a=%d b=%d" % (
            int(r["seq"]), int(r["ts_us"]), int(r["strand_id"]), ch, lvl, sch,
            int(r["payload"]["words"]["a"]), int(r["payload"]["words"]["b"]))


def build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("urbi")
    pp.add_printer("UValue", "^UValue$", UValuePrinter)
    pp.add_printer("UStrand", "^UStrand$", UStrandPrinter)
    pp.add_printer("UTraceRecord", "^UTraceRecord$", UTraceRecordPrinter)
    return pp


# ---- walkers ---------------------------------------------------------------

def resolve_vm(arg):
    """Return a 'UVM *' gdb.Value from the command arg (default: `vm`)."""
    expr = arg.strip() if arg and arg.strip() else "vm"
    v = gdb.parse_and_eval(expr)
    if v.type.strip_typedefs().code == gdb.TYPE_CODE_PTR:
        return v
    return v.address  # `vm` is a UVM, take &vm


def ptr(v):
    return int(v)


class UrbiStrands(gdb.Command):
    """urbi-strands [VM] - list every live strand across all realms."""
    def __init__(self):
        super(UrbiStrands, self).__init__("urbi-strands", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        vm = resolve_vm(arg)
        ready = set()
        s = vm["ready_head"]
        while ptr(s):
            ready.add(ptr(s))
            s = s["ready_next"]
        print("%-6s %-18s %-18s %s" % ("id", "addr", "state", "on-runq"))
        n = 0
        realm = vm["realms_head"]
        while ptr(realm):
            st = realm["strands_head"]
            while ptr(st):
                a = ptr(st)
                print("%-6d 0x%016x %-18s %s" % (
                    a & 0xFFFF, a, strand_state_str(int(st["state"]) & 0xFF),
                    "yes" if a in ready else ""))
                st = st["next_in_realm"]
                n += 1
            realm = realm["next_in_vm"]
        print("(%d strand(s))" % n)


class UrbiHandles(gdb.Command):
    """urbi-handles [VM] - list in-use host-handle table slots."""
    def __init__(self):
        super(UrbiHandles, self).__init__("urbi-handles", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        vm = resolve_vm(arg)
        cap = int(vm["handle_table_cap"])
        tbl = vm["handle_table"]
        if not ptr(tbl) or cap == 0:
            print("handle table: empty (cap=%d)" % cap)
            return
        live = 0
        for i in range(cap):
            val = tbl[i]
            if int(val["kind"]) != 0:  # non-nil slot is in use
                print("  [%d] %s" % (i, UValuePrinter(val).to_string()))
                live += 1
        print("handle table: %d live / cap %d (next_id=%d)" % (
            live, cap, int(vm["handle_table_next_id"])))


# ---- v0.11.3 memory-debug helpers ------------------------------------------
# Empirical layout (verified on the real -g binary, v0.11.3):
#   sizeof(UAllCellsNode)=64, sizeof(UMemDebug)=960. The walkers read fields by
#   name via debug info, so they tolerate layout changes as long as the names hold.

def _node_head(vm):
    """vm->all_cells_head is a UCell* laundered from UAllCellsNode*; recast."""
    try:
        t = gdb.lookup_type("UAllCellsNode").pointer()
    except gdb.error:
        return None
    head = vm["all_cells_head"]
    if not ptr(head):
        return None
    return head.cast(t)


def _has_owner(node):
    try:
        node["owner_ret"]
        return True
    except gdb.error:
        return False


def _sym(addr):
    if not addr:
        return "-"
    try:
        s = gdb.execute("info symbol 0x%x" % int(addr), to_string=True).strip()
        return s.split(" in ")[0]
    except gdb.error:
        return "0x%x" % int(addr)


class UrbiHeap(gdb.Command):
    """urbi-heap [VM] - GC stats + full live-cell walk (URBI_GC_INCREMENTAL)."""
    def __init__(self):
        super(UrbiHeap, self).__init__("urbi-heap", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        vm = resolve_vm(arg)
        for f in ("gc_live_bytes", "gc_total_allocated", "gc_threshold", "gc_debt",
                  "gc_phase", "gc_cycles", "gc_slices", "last_gc_us", "total_gc_us"):
            try:
                print("  %-20s %d" % (f, int(vm[f])))
            except gdb.error:
                print("  %-20s <n/a>" % f)
        print("  %-20s %d (cap %d)" % ("handles_next_id",
              int(vm["handle_table_next_id"]), int(vm["handle_table_cap"])))
        node = _node_head(vm)
        if node is None:
            print("  (no all-cells walk: UAllCellsNode type absent)")
            return
        by_type = {}
        total = 0
        n = 0
        while ptr(node) and n < 100000:
            cell = node["cell"]
            tt = int(cell["type_tag"]) if ptr(cell) else -1
            sz = int(node["size"])
            ent = by_type.setdefault(tt, [0, 0])
            ent[0] += 1
            ent[1] += sz
            total += sz
            n += 1
            node = node["next"]
        print("  live cells: %d (%d bytes)" % (n, total))
        for tt in sorted(by_type, key=lambda k: -by_type[k][1]):
            print("    type %-3d  %6d cells  %8d bytes" % (tt, by_type[tt][0], by_type[tt][1]))


class UrbiAllocs(gdb.Command):
    """urbi-allocs [VM] [N] - top-N allocation sites by bytes (needs URBI_MEM_DEBUG)."""
    def __init__(self):
        super(UrbiAllocs, self).__init__("urbi-allocs", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        parts = arg.split() if arg else []
        topn = 20
        if parts and parts[-1].isdigit():
            topn = int(parts[-1])
            parts = parts[:-1]
        vm = resolve_vm(" ".join(parts))
        node = _node_head(vm)
        if node is None or not _has_owner(node):
            print("urbi-allocs: no owner data (build with URBI_MEM_DEBUG=1)")
            return
        sites = {}
        n = 0
        while ptr(node) and n < 100000:
            ret = int(node["owner_ret"]) if ptr(node["owner_ret"]) else 0
            ent = sites.setdefault(ret, [0, 0])
            ent[0] += 1
            ent[1] += int(node["size"])
            n += 1
            node = node["next"]
        print("%-44s %8s %10s" % ("alloc site", "cells", "bytes"))
        for ret in sorted(sites, key=lambda k: -sites[k][1])[:topn]:
            print("%-44s %8d %10d" % (_sym(ret), sites[ret][0], sites[ret][1]))


class UrbiLeaks(gdb.Command):
    """urbi-leaks [VM] - live host handles + sites, pinned cells, heap-lock violations."""
    def __init__(self):
        super(UrbiLeaks, self).__init__("urbi-leaks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        vm = resolve_vm(arg)
        cap = int(vm["handle_table_cap"])
        tbl = vm["handle_table"]
        owners = None
        ocap = 0
        try:
            md = vm["memdbg"]
            if ptr(md):
                owners = md["handle_owner"]
                ocap = int(md["handle_owner_cap"])
        except gdb.error:
            pass
        live = 0
        for i in range(cap):
            if ptr(tbl) and int(tbl[i]["kind"]) != 0:
                site = "-"
                if owners is not None and ptr(owners) and i < ocap:
                    site = _sym(int(owners[i]["owner_ret"]))
                print("  handle[%d] %s  created-at %s" % (
                    i, UValuePrinter(tbl[i]).to_string(), site))
                live += 1
        print("  live handles: %d" % live)
        node = _node_head(vm)
        pinned = 0
        while node is not None and ptr(node) and pinned < 100000:
            cell = node["cell"]
            if ptr(cell) and (int(cell["gc_byte"]) & 0x10):  # UGC_IS_PINNED
                site = _sym(int(node["owner_ret"])) if _has_owner(node) else "-"
                print("  pinned cell @0x%x type %d  owner %s" % (
                    int(cell), int(cell["type_tag"]), site))
                pinned += 1
            node = node["next"]
        print("  pinned cells: %d" % pinned)
        try:
            md = vm["memdbg"]
            if ptr(md):
                print("  heap-lock violations: %d" % int(md["hlv_count"]))
        except gdb.error:
            pass


class UrbiTrace(gdb.Command):
    """urbi-trace [VM] [N] - decode the last N trace-ring records (default 32)."""
    def __init__(self):
        super(UrbiTrace, self).__init__("urbi-trace", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        parts = arg.split() if arg else []
        n = 32
        if parts and parts[-1].isdigit():
            n = int(parts[-1])
            parts = parts[:-1]
        vm = resolve_vm(" ".join(parts))
        try:
            ts = vm["trace"]
        except gdb.error:
            print("trace: this build has no trace subsystem (URBI_TRACE off)")
            return
        if not ptr(ts):
            print("trace: ring not allocated (no channel enabled)")
            return
        ring = ts["ring"]
        depth = ring.type.sizeof // ring.type.target().sizeof
        count = int(ts["count"])
        head = int(ts["head"])
        show = min(n, count)
        start = (head - show) % depth
        for k in range(show):
            rec = ring[(start + k) % depth]
            print("  " + UTraceRecordPrinter(rec).to_string())
        print("(%d of %d live records; depth %d)" % (show, count, depth))


class UrbiDump(gdb.Command):
    """urbi-dump [VM] - emergency dump: strands + heap + trace tail."""
    def __init__(self):
        super(UrbiDump, self).__init__("urbi-dump", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        a = arg or ""
        print("=== urbi-dump: strands ===")
        gdb.execute("urbi-strands " + a)
        print("=== urbi-dump: heap ===")
        gdb.execute("urbi-heap " + a)
        print("=== urbi-dump: allocs ===")
        gdb.execute("urbi-allocs " + a)
        print("=== urbi-dump: leaks ===")
        gdb.execute("urbi-leaks " + a)
        print("=== urbi-dump: trace tail ===")
        gdb.execute("urbi-trace " + a)


try:
    gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer(), replace=True)
except Exception as _e:  # pragma: no cover - registration is best-effort
    print("urbi.py: pretty-printer registration skipped:", _e)

UrbiStrands()
UrbiHandles()
UrbiHeap()
UrbiAllocs()
UrbiLeaks()
UrbiTrace()
UrbiDump()
print("urbi-embedded GDB helpers loaded: urbi-strands, urbi-handles, urbi-heap, "
      "urbi-allocs, urbi-leaks, urbi-trace, urbi-dump")
