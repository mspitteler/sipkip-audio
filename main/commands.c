#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <ctype.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"

#include "commands.h"
#include "vfs-acceptor.h"
#include "sipkip-audio.h"
#include "xmodem.h"
#include "utils.h"

static const char *const TAG = "commands";

#define DECL_COMMAND(command_name)                                                                                 \
    IMPL_COMMAND(command_name);
#define DEF_COMMAND(command_name, command_params, command_usage)                                                   \
    {                                                                                                              \
        .name = #command_name,                                                                                     \
        .usage = "Usage: "#command_name" "command_params"\n\t"command_usage"\n",                                   \
        .fn = &command_##command_name                                                                              \
    },
    
#define IMPL_COMMAND(command_name)                                                                                 \
    static esp_err_t command_##command_name(int argc, char **argv)

DECL_COMMAND(help) DECL_COMMAND(rx) DECL_COMMAND(rm) DECL_COMMAND(mv) DECL_COMMAND(cp) DECL_COMMAND(speak)
DECL_COMMAND(mkdir) DECL_COMMAND(rmdir) DECL_COMMAND(ls) DECL_COMMAND(cwd) DECL_COMMAND(pwd) DECL_COMMAND(du)

const struct vfs_commands commands[] = {
    DEF_COMMAND(help, "[command_name]", "Prints help information about command [command_name].")
    DEF_COMMAND(rx, "[filename]", "Starts receiving file [filename] with protocol XMODEM.")
    DEF_COMMAND(rm, "[filename]", "Removes file [filename].")
    DEF_COMMAND(mv, "[src_name] [dst_name]", "Moves file or directory [src_name] to [dst_name].")
    DEF_COMMAND(cp, "[src_filename] [dst_filename]", "Copies file [src_filename] to [dst_filename].")
    DEF_COMMAND(speak, "[opus_filename] [opus_packets_filename]", "Plays the opus stream contained in [opus_filename]")
    DEF_COMMAND(mkdir, "[dirname]", "Creates directory [dirname].")
    DEF_COMMAND(rmdir, "[dirname]", "Removes directory [dirname] (only if it is empty).")
    DEF_COMMAND(ls, "[name]", "Lists files and directories in [name], or only [name] if [name] is a file.")
    DEF_COMMAND(cwd, "[dirname]", "Change current working directory to [dirname]")
    DEF_COMMAND(pwd, "", "Prints current working directory.")
    DEF_COMMAND(du, "", "Prints the disk usage and total capacity.")
    {0}
};

IMPL_COMMAND(help) {
    /* General help. */
    if (argc == 1) {
        dprintf(spp_fd, "Available commands:\n");
        for (const struct vfs_commands *command = commands; command->name; command++)
             dprintf(spp_fd, "\t* %s\n", command->name);
        dprintf(spp_fd, "Type `help [command_name]' for more information about a specific command.\n");
        return ESP_OK;
    /* Help for specific command. */
    } else if (argc == 2) {
        const struct vfs_commands *command = find_command(argv[1]);
        if (!command) {
            dprintf(spp_fd, "Unknown command: %s!\n", argv[1]);
            return ESP_OK;
        }

        dprintf(spp_fd, "%s", command->usage);
        return ESP_OK;
    }
    /* Wrong amount of arguments. */
    return ESP_ERR_INVALID_ARG;
}

IMPL_COMMAND(rx) {
    int littlefs_fd;
    bool remove_file = false;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;

    if (strstr(argv[1], LITTLEFS_BASE_PATH"/") != argv[1]) {
        dprintf(spp_fd, "Invalid file name: %s, doesn't start with %s\n", argv[1], LITTLEFS_BASE_PATH"/");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Detected valid filename: %s", argv[1]);

    littlefs_fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL);
    if (littlefs_fd < 0) {
        dprintf(spp_fd, "Failed to open file %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }

    if (xmodem_receiver_start(spp_fd, littlefs_fd) != ESP_OK) {
        dprintf(spp_fd, "Failed to receive file %s using XMODEM\n", argv[1]);
        remove_file = true;
    }

    /* Close file, and unlink if the transfer failed. */
    close(littlefs_fd);
    if (remove_file)
        command_rm(argc, argv);
    return ESP_OK;
}

IMPL_COMMAND(rm) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Removing file: %s", argv[1]);
    
    ret = remove(argv[1]);
    if (ret) {
        dprintf(spp_fd, "Failed to remove file %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(mv) {
    int ret;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Moving file: %s to %s", argv[1], argv[2]);
    
    ret = rename(argv[1], argv[2]);
    if (ret) {
        dprintf(spp_fd, "Failed to move file %s to %s: %s\n", argv[1], argv[2], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(cp) {
    int ret;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Copying file: %s to %s", argv[1], argv[2]);
    
    ret = copy_file(argv[1], argv[2]);
    if (ret) {
        dprintf(spp_fd, "Failed to copy file %s to %s: %s\n", argv[1], argv[2], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(speak) {
    unsigned int file_opus_packets_len;
    
    if (argc != 3)
        /* Wrong amount of arguments, or first and second argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Playing opus file: %s, with opus packets file: %s", argv[1], argv[2]);
    
    FILE *file_opus = fopen(argv[1], "rb");
    if (!file_opus) {
        dprintf(spp_fd, "Failed to open file %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    FILE *file_opus_packets = fopen(argv[2], "rb");
    if (!file_opus_packets) {
        dprintf(spp_fd, "Failed to open file %s: %s\n", argv[2], strerror(errno));
        goto exit;
    }
    
    fseek(file_opus_packets, 0, SEEK_END);
    file_opus_packets_len = ftell(file_opus_packets);
    fseek(file_opus_packets, 0, SEEK_SET);
    
    exit_dac_write_opus_loop = true;    
    DAC_WRITE_OPUS(file_opus, file);
    
exit:
    if (file_opus)
        fclose(file_opus);
    if (file_opus_packets)
        fclose(file_opus_packets);
    
    return ESP_OK;
}

IMPL_COMMAND(mkdir) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Creating directory: %s", argv[1]);
    
    ret = mkdir(argv[1], 0777);
    if (ret) {
        dprintf(spp_fd, "Failed to create directory %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(rmdir) {
    int ret;
    
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Removing directory: %s\n", argv[1]);
    
    ret = rmdir(argv[1]);
    if (ret) {
        dprintf(spp_fd, "Failed to remove directory %s: %s\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(ls) {
    char buffer[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    if (argc == 1) {
        if (!getcwd(buffer, sizeof(buffer) / sizeof(*buffer))) {
            dprintf(spp_fd, "Couldn't determine current working directory: %s!\n", strerror(errno));
            return ESP_OK;
        }
    } else if (argc == 2) {
        strncpy(buffer, argv[1], sizeof(buffer) / sizeof(*buffer));
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(buffer);
    if (!dir) {
        dprintf(spp_fd, "Couldn't open directory %s: %s!\n", buffer, strerror(errno));
        return ESP_OK;
    }

    for (;;) {
        struct dirent* de = readdir(dir);
        if (!de)
            break;
        
        dprintf(spp_fd, "%s%s\n", de->d_name, &"/"[de->d_type != DT_DIR]); /* Print trailing `/' if it's a directory. */
    }

    closedir(dir);
    return ESP_OK;
}

IMPL_COMMAND(cwd) {
    if (argc != 2)
        /* Wrong amount of arguments, or first argument isn't a path. */
        return ESP_ERR_INVALID_ARG;
    
    if (chdir(argv[1])) {
        dprintf(spp_fd, "Couldn't change current working directory to %s: %s!\n", argv[1], strerror(errno));
        return ESP_OK;
    }
    
    return ESP_OK;
}

IMPL_COMMAND(pwd) {
    char buffer[(CONFIG_LITTLEFS_OBJ_NAME_LEN + 1) * LITTLEFS_MAX_DEPTH];
    
    if (argc != 1)
        return ESP_ERR_INVALID_ARG;
    
    if (!getcwd(buffer, sizeof(buffer) / sizeof(*buffer))) {
        dprintf(spp_fd, "Couldn't determine current working directory: %s!\n", strerror(errno));
        return ESP_OK;
    }
    dprintf(spp_fd, "%s\n", buffer);
    
    return ESP_OK;
}

IMPL_COMMAND(du) {
    int ret;
    
    if (argc != 1)
        return ESP_ERR_INVALID_ARG;
    
    size_t total = 0UL, used = 0UL;
    ret = esp_littlefs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        dprintf(spp_fd, "Failed to get LITTLEFS partition information (%s).\n", esp_err_to_name(ret));
    } else {
        dprintf(spp_fd, "Used: %lu, total: %lu\n", used, total);
    }
    return ESP_OK;
}
