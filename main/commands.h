#ifndef COMMANDS_H
#define COMMANDS_H

#include <string.h>

#include "esp_err.h"

struct vfs_commands {
    const char *name;
    const char *usage;
    esp_err_t (*fn)(int argc, char **argv);
};

extern const struct vfs_commands commands[];

static inline const struct vfs_commands *find_command(const char *name) {
    const struct vfs_commands *command;
    for (command = commands; command->name && strcmp(name, command->name); command++);

    if (!command->name)
        return NULL;
    return command;
}

#endif /* COMMANDS_H */
