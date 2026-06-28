#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "wasm_export.h"

#include "services/iwasm/iwasm.h"

LOG_MODULE_REGISTER(iwasm, LOG_LEVEL_INF);

static bool runtime_ready;

int iwasm_init(void)
{
	RuntimeInitArgs init_args;

	memset(&init_args, 0, sizeof(init_args));

	if (!wasm_runtime_full_init(&init_args)) {
		LOG_ERR("Failed to initialize WAMR runtime");
		return -EIO;
	}

	runtime_ready = true;
	LOG_INF("iwasm runtime initialized");
	return 0;
}

static int iwasm_exec_file(const char *path)
{
	struct fs_file_t file;
	ssize_t file_size;
	uint8_t *buf = NULL;
	wasm_module_t module = NULL;
	wasm_module_inst_t module_inst = NULL;
	char error_buf[128];
	int ret = -1;

	if (!runtime_ready) {
		LOG_ERR("Runtime not initialized");
		return -EIO;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret != 0) {
		LOG_ERR("Cannot open %s (err %d)", path, ret);
		return ret;
	}

	ret = fs_seek(&file, 0, FS_SEEK_END);
	if (ret != 0) {
		LOG_ERR("Cannot seek %s (err %d)", path, ret);
		fs_close(&file);
		return ret;
	}

	file_size = fs_tell(&file);
	if (file_size <= 0) {
		LOG_ERR("File %s is empty or unreadable", path);
		fs_close(&file);
		return -EINVAL;
	}

	fs_seek(&file, 0, FS_SEEK_SET);

	buf = (uint8_t *)malloc(file_size);
	if (!buf) {
		LOG_ERR("Out of memory reading %s (%d bytes)", path, file_size);
		fs_close(&file);
		return -ENOMEM;
	}

	ret = fs_read(&file, buf, file_size);
	fs_close(&file);
	if (ret < 0) {
		LOG_ERR("Failed to read %s (err %d)", path, ret);
		goto cleanup_buf;
	}

	error_buf[0] = '\0';
	module = wasm_runtime_load(buf, (uint32_t)file_size, error_buf,
				   sizeof(error_buf));
	if (!module) {
		LOG_ERR("Load failed: %s", error_buf);
		ret = -EINVAL;
		goto cleanup_buf;
	}

	module_inst = wasm_runtime_instantiate(module, 8192, 8192, error_buf,
					       sizeof(error_buf));
	if (!module_inst) {
		LOG_ERR("Instantiate failed: %s", error_buf);
		ret = -EINVAL;
		goto cleanup_module;
	}

	if (!wasm_application_execute_main(module_inst, 0, NULL)) {
		const char *exc = wasm_runtime_get_exception(module_inst);

		LOG_ERR("Execute failed: %s", exc ? exc : "unknown");
		ret = -EIO;
	} else {
		LOG_INF("Executed %s successfully", path);
		ret = 0;
	}

	wasm_runtime_deinstantiate(module_inst);
cleanup_module:
	wasm_runtime_unload(module);
cleanup_buf:
	free(buf);
	return ret;
}

static int cmd_iwasm_exec(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: iwasm exec <file>");
		return -EINVAL;
	}

	int ret = iwasm_exec_file(argv[1]);

	if (ret != 0) {
		shell_error(sh, "iwasm exec failed: %d", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(iwasm_subcmds,
	SHELL_CMD(exec, NULL, "Execute a WASM/AOT file: iwasm exec <file>",
		  cmd_iwasm_exec),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(iwasm, &iwasm_subcmds, "iwasm runtime commands", NULL);
