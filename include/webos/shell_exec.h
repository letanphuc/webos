/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEBOS_INCLUDE_SHELL_EXEC_H_
#define WEBOS_INCLUDE_SHELL_EXEC_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Execute one command against the shared dummy backend and copy bounded output. */
int webos_shell_execute(const char* command, char* output, size_t capacity, size_t* output_len, bool* truncated);

/** Push input to an attached command and return the backend's current bounded output. */
int webos_shell_push_input(const char* input, size_t input_len, char* output, size_t capacity, size_t* output_len,
                           bool* truncated);

/** Clear the shared backend output. */
void webos_shell_clear_output(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBOS_INCLUDE_SHELL_EXEC_H_ */
