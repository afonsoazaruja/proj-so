#include "operations.h"
#include "config.h"
#include "state.h"
#include "betterassert.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count = 16,
        .block_size = 1024,
    };
    return params;
}

int tfs_init(tfs_params const *params_ptr) {
    tfs_params params;
    if (params_ptr != NULL) {
        params = *params_ptr;
    } else {
        params = tfs_default_params();
    }

    if (state_init(params) != 0) {
        return -1;
    }

    // create root inode
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

/**
 * Looks for a file.
 *
 * Note: as a simplification, only a plain directory space (root directory only)
 * is supported.
 *
 * Input:
 *   - name: absolute path name
 *   - root_inode: the root directory inode
 * Returns the inumber of the file, -1 if unsuccessful.
 */
static int tfs_lookup(char const *name, inode_t const *root_inode) {
    //TODO assert that root_inode is the root directory
    ALWAYS_ASSERT(root_inode != NULL, "tfs_lookup: root dir inode must exist");
    if (root_inode != inode_get(ROOT_DIR_INUM)) return -1;
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(root_inode, name);
}

int tfs_open(char const *name, tfs_file_mode_t mode) {
    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_open: root dir inode must exist");
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset;

    if (inum >= 0) {
        // The file already exists
        inode_t *inode = inode_get(inum);
        ALWAYS_ASSERT(inode != NULL,
                      "tfs_open: directory files must have an inode");

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            printf("no space\n");
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            inode_delete(inum);
            printf("no space dir\n");
            return -1; // no space in directory
        }

        offset = 0;
    } else {
        return -1;
    }

    int fhandle = add_to_open_file_table(inum, offset); 
    inode_t *inode_file = inode_get(inum);

    if (inode_file->i_node_type == T_SYM_LINK) {
        char buffer[1024];
        char res[1024];
        ssize_t sizeRead;
        int start = 0;
        do {
            sizeRead = tfs_read(fhandle, buffer, sizeof(buffer));
            if (sizeRead == -1) {
                return -1;
            }
            int k = 0;
            for(; k < sizeRead; k++) {
                res[k+start] = buffer[k];
            }
            start += k + 1;
        } while(sizeRead > 0);

        if ((inum = tfs_lookup(res, root_dir_inode)) == -1) {
            return -1;
        }
        return tfs_open(res, mode);
    }

    // Finally, add entry to the open file table and return the corresponding
    // handle
    return fhandle;

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link_name) {
    //TODO O que fazer se o link_name já existe????
    int fhandle = tfs_open(link_name, TFS_O_CREAT | TFS_O_TRUNC);
    
    if (fhandle == -1) {
        return -1;
    }
    
    inode_t * root_dir_inode = inode_get(ROOT_DIR_INUM);
    int inumber_link = tfs_lookup(link_name, root_dir_inode);
    inode_t *inode_link = inode_get(inumber_link);
    inode_link->i_node_type = T_SYM_LINK; // alterar tipo de inode para soft_link

    ssize_t sizeWritten;
    do {
        sizeWritten = tfs_write(fhandle, target, sizeof(target));
        if (sizeWritten == -1) {
            return -1;
        }

    }while(sizeWritten);

    if (tfs_close(fhandle) == -1) {
        return -1;
    }

    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    //TODO incrementar hard_link count????
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);

    int inumber_target = tfs_lookup(target, root_dir_inode);
    if (inumber_target == -1) {
        return -1;
    }
    
    add_dir_entry(root_dir_inode, link_name + 1, inumber_target);
    inode_t *inode_target = inode_get(inumber_target);
    inode_target->link_count++;

    return 0;
}

int tfs_close(int fhandle) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1; // invalid fd
    }

    remove_from_open_file_table(fhandle);

    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    //  From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_write: inode of open file deleted");

    // Determine how many bytes to write
    size_t block_size = state_block_size();
    if (to_write + file->of_offset > block_size) {
        to_write = block_size - file->of_offset;
    }

    if (to_write > 0) {
        if (inode->i_size == 0) {
            // If empty file, allocate new block
            int bnum = data_block_alloc();
            if (bnum == -1) {
                return -1; // no space
            }
            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write
        memcpy(block + file->of_offset, buffer, to_write);

        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }

    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    // From the open file table entry, we get the inode
    inode_t const *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");

    // Determine how many bytes to read
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_read: data block deleted mid-read");

        // Perform the actual read
        memcpy(buffer, block + file->of_offset, to_read);
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_read;
    }

    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM); 
    
    int inumber = tfs_lookup(target, root_dir_inode);
    inode_t *inode = inode_get(inumber);
    
    if (clear_dir_entry(root_dir_inode, target) == -1) {
        return -1;
    }
    if (inode->link_count == 1) {
        data_block_free(inode->i_data_block);
    }
    
    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    FILE *fp = fopen(source_path, "r");
    
    if (fp == NULL) {
        return -1;
    }

    char buffer[1024];

    size_t sizeRead = fread(buffer, sizeof(char), sizeof(buffer), fp);
    if (sizeRead == -1) {
        return -1;
    }

    int fhandle = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
    if (fhandle == -1) {
        return -1;
    }

    ssize_t sizeWritten;
    while (sizeRead > 0) {
        sizeWritten = tfs_write(fhandle, buffer, sizeRead);
        
        if (sizeWritten == -1) return -1;
        
        sizeRead = fread(buffer, sizeof(char), sizeof(buffer), fp);
        if (sizeRead == -1) return -1;
    }

    if (tfs_close(fhandle) == -1) {
        return -1;
    }
    
    fclose(fp);

    return 0;
}