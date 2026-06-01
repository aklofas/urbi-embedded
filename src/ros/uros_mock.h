/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UROS_MOCK_H
#define UROS_MOCK_H
#ifdef URBI_ENABLE_ROS2
#include "ros/uros_transport.h"
#include "urbi/types.h"
struct UVM;

/* Populate *tp with the mock transport function pointers and allocate state. */
void uros_mock_init(URosTransport *tp);

/* Free the mock state allocated by uros_mock_init. */
void uros_mock_free(URosTransport *tp);

/* Inject a pre-built message object into the mock's incoming queue for the
 * given subscription handle.  The object is marshaled into an internal blob
 * so the mock can unmarshal it back during spin().  The type string must
 * match a registered URosMsgType. */
void uros_mock_inject_obj(void *self, struct UVM *vm,
                          uint32_t sub, const char *type, UValue msg_obj);

/* Low-level raw-bytes inject (used by ros_inject_int32_method which builds
 * the struct itself).  Prefer uros_mock_inject_obj for new code. */
void uros_mock_inject(void *self, uint32_t sub, const void *bytes, size_t len);

/* Query: did a publish on pub_handle capture anything?  If so fills *out_bytes
 * + *out_len (transport-owned buffer, valid until next publish on same handle)
 * and returns 1.  Returns 0 if nothing was published. */
int  uros_mock_last_published(void *self, uint32_t pub,
                              const void **out_bytes, size_t *out_len);

/* Return the ROS type name registered for publisher handle pub (e.g.
 * "std_msgs/Float64").  Returns NULL if the handle is out of range.
 * The pointer is owned by the mock state and valid for the mock's lifetime. */
const char *uros_mock_pub_type(void *self, uint32_t pub);

/* Return the ROS type name registered for subscription handle sub.
 * Returns NULL if the handle is out of range.
 * The pointer is owned by the mock state and valid for the mock's lifetime. */
const char *uros_mock_sub_type(void *self, uint32_t sub);

#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_MOCK_H */
