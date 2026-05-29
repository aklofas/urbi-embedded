/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UROS_MOCK_H
#define UROS_MOCK_H
#ifdef URBI_ENABLE_ROS2
#include "ros/uros_transport.h"
void uros_mock_init(URosTransport *tp);
void uros_mock_free(URosTransport *tp);
void uros_mock_inject(void *self, uint32_t sub, const void *bytes, size_t len);
int  uros_mock_last_published(void *self, uint32_t pub,
                              const void **out_bytes, size_t *out_len);
#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_MOCK_H */
