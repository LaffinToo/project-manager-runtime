#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <archive.h>
#include <archive_entry.h>
#include <time.h>
#include <errno.h>

#define MAX_EXCLUDES 1024
#define PATH_SIZE 4096
#define NAME_SIZE 256

typedef struct {
    char name[NAME_SIZE];
} ExcludeList;

typedef struct {
    char project_root[PATH_SIZE];
    char truth_dir[PATH_SIZE];
    char archive_path[PATH_SIZE];
    char backup_root[PATH_SIZE];
    char config_path[PATH_SIZE];
} ProjectLayout;

void join_path(char *dst, size_t size, const char *p1, const char *p2);
void native_copy_dir(const char *src, const char *dst);
int native_copy_file(const char *src, const char *dst);

static int path_is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void init_project_layout(const char *storage_base, const char *project_name, ProjectLayout *layout) {
    memset(layout, 0, sizeof(*layout));
    snprintf(layout->project_root, sizeof(layout->project_root), "%s/%s", storage_base, project_name);
    snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s/truth", layout->project_root);
    snprintf(layout->archive_path, sizeof(layout->archive_path), "%s/project.tar.gz", layout->project_root);
    snprintf(layout->backup_root, sizeof(layout->backup_root), "%s/truth_backups", layout->project_root);
    snprintf(layout->config_path, sizeof(layout->config_path), "%s/project.config", layout->project_root);

    if (path_is_directory(layout->truth_dir)) return;

    char candidate[PATH_SIZE];
    snprintf(candidate, sizeof(candidate), "%s/vault", layout->project_root);
    if (path_is_directory(candidate)) {
        snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s", candidate);
        return;
    }

    snprintf(candidate, sizeof(candidate), "%s/files", layout->project_root);
    if (path_is_directory(candidate)) {
        snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s", candidate);
        return;
    }

    if (path_is_directory(layout->project_root)) {
        snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s", layout->project_root);
    }
}

static int migrate_project_layout(const char *storage_base, const char *project_name, ProjectLayout *layout) {
    init_project_layout(storage_base, project_name, layout);

    char truth_dir[PATH_SIZE];
    snprintf(truth_dir, sizeof(truth_dir), "%s/truth", layout->project_root);
    if (!path_is_directory(truth_dir)) {
        if (mkdir(truth_dir, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    if (strcmp(layout->truth_dir, truth_dir) == 0) {
        snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s", truth_dir);
        return 0;
    }

    snprintf(layout->truth_dir, sizeof(layout->truth_dir), "%s", truth_dir);
    if (!path_is_directory(layout->truth_dir)) {
        return -1;
    }

    if (!path_is_directory(layout->project_root)) {
        return 0;
    }

    DIR *d = opendir(layout->project_root);
    if (!d) return 0;

    struct dirent *p;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;
        if (strcmp(p->d_name, "truth") == 0 || strcmp(p->d_name, "truth_backups") == 0 ||
            strcmp(p->d_name, "project.tar.gz") == 0 || strcmp(p->d_name, "project.config") == 0) continue;

        char src[PATH_SIZE], dst[PATH_SIZE];
        join_path(src, sizeof(src), layout->project_root, p->d_name);
        join_path(dst, sizeof(dst), truth_dir, p->d_name);

        struct stat st;
        if (stat(src, &st) != 0) continue;
        if (strcmp(src, layout->truth_dir) == 0) continue;
        if (S_ISDIR(st.st_mode)) {
            native_copy_dir(src, dst);
        } else if (S_ISREG(st.st_mode)) {
            native_copy_file(src, dst);
        }
    }
    closedir(d);
    return 0;
}

void get_backup_timestamp(char *buf, size_t max_size) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    // Generates a clean format: YYYYMMDD_HHMMSS
    strftime(buf, max_size, "%Y%m%d_%H%M%S", tm_info);
}

void join_path(char *dst, size_t size, const char *p1, const char *p2) {
    snprintf(dst, size, "%s/%s", p1, p2);
}

int workspace_relative_path(const char *workspace_root, const char *input_path, char *out, size_t out_size) {
    char resolved_input[PATH_SIZE];
    char resolved_root[PATH_SIZE];

    if (realpath(input_path, resolved_input) == NULL) {
        strncpy(resolved_input, input_path, sizeof(resolved_input) - 1);
        resolved_input[sizeof(resolved_input) - 1] = '\0';
    }

    if (realpath(workspace_root, resolved_root) == NULL) {
        strncpy(resolved_root, workspace_root, sizeof(resolved_root) - 1);
        resolved_root[sizeof(resolved_root) - 1] = '\0';
    }

    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_input, resolved_root, root_len) == 0) {
        const char *rel = resolved_input + root_len;
        if (*rel == '/') rel++;
        if (*rel == '\0') {
            snprintf(out, out_size, ".");
        } else {
            snprintf(out, out_size, "%s", rel);
        }
        return 0;
    }

    const char *base = strrchr(input_path, '/');
    if (base) {
        snprintf(out, out_size, "%s", base + 1);
    } else {
        snprintf(out, out_size, "%s", input_path);
    }
    return 1;
}

// Modified to accept a flag: 1 = delete the folder itself, 0 = keep root folder, delete contents only
void native_rmdir_recursive(const char *path, int delete_root) {
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *p;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;

        char buf[PATH_SIZE];
        join_path(buf, sizeof(buf), path, p->d_name);

        struct stat statbuf;
        if (!stat(buf, &statbuf)) {
            if (S_ISDIR(statbuf.st_mode)) {
                native_rmdir_recursive(buf, 1); // Subfolders are always fully deleted
            } else {
                unlink(buf);
            }
        }
    }
    closedir(d);
    
    if (delete_root) {
        rmdir(path);
    }
}

int native_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in); fclose(out);
    return 0;
}

int ensure_parent_directory(const char *path) {
    char tmp[PATH_SIZE];
    char *slash;
    size_t len = strlen(path);

    if (len >= sizeof(tmp)) return -1;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    slash = strrchr(tmp, '/');
    if (!slash) return 0;

    *slash = '\0';
    if (strlen(tmp) == 0) return 0;

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void native_copy_dir(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return;

    mkdir(dst, 0755);
    struct dirent *p;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;

        // FIXED: Ensuring proper array brackets are used to allocate real memory buffers
        char src_buf[PATH_SIZE];
        char dst_buf[PATH_SIZE];
        
        join_path(src_buf, sizeof(src_buf), src, p->d_name);
        join_path(dst_buf, sizeof(dst_buf), dst, p->d_name);

        struct stat statbuf;
        if (!stat(src_buf, &statbuf)) {
            if (S_ISDIR(statbuf.st_mode)) {
                native_copy_dir(src_buf, dst_buf);
            } else {
                native_copy_file(src_buf, dst_buf);
            }
        }
    }
    closedir(d);
}

int extract_tar_gz(const char *archive_path, const char *dest_dir) {
    struct archive *a = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(a); archive_write_free(ext);
        return -1;
    }
    char old_cwd[PATH_SIZE];
    if (!getcwd(old_cwd, sizeof(old_cwd))) { 
        archive_read_free(a); archive_write_free(ext);
        return -1; 
    }
    chdir(dest_dir);
    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (archive_write_header(ext, entry) == ARCHIVE_OK) {
            const void *buff;
            size_t size;
            int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                archive_write_data_block(ext, buff, size, offset);
            }
        }
        archive_write_finish_entry(ext);
    }
    archive_read_close(a); archive_read_free(a);
    archive_write_close(ext); archive_write_free(ext);
    chdir(old_cwd);
    return 0;
}

void add_to_archive_recursive(struct archive *a, const char *root_path, const char *current_rel_path) {
    char absolute_path[PATH_SIZE];
    if (strlen(current_rel_path) == 0) {
        strncpy(absolute_path, root_path, sizeof(absolute_path));
    } else {
        join_path(absolute_path, sizeof(absolute_path), root_path, current_rel_path);
    }
    DIR *d = opendir(absolute_path);
    if (!d) return;
    struct dirent *p;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;
        
        // Safety: Exclude 'truth' directory references from being bundled into the tarball
        if (strlen(current_rel_path) == 0 && strcmp(p->d_name, "truth") == 0) continue;

        char next_rel[PATH_SIZE];
        if (strlen(current_rel_path) == 0) {
            strncpy(next_rel, p->d_name, sizeof(next_rel));
        } else {
            join_path(next_rel, sizeof(next_rel), current_rel_path, p->d_name);
        }
        char next_abs[PATH_SIZE];
        join_path(next_abs, sizeof(next_abs), root_path, next_rel);
        struct stat st;
        if (stat(next_abs, &st) != 0) continue;
        
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, next_rel);
        archive_entry_copy_stat(entry, &st);
        archive_write_header(a, entry);

        if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(next_abs, "rb");
            if (f) {
                char chunk[8192];
                size_t len;
                while ((len = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                    archive_write_data(a, chunk, len);
                }
                fclose(f);
            }
            archive_entry_free(entry);
        } 
        else if (S_ISDIR(st.st_mode)) {
            archive_entry_free(entry);
            add_to_archive_recursive(a, root_path, next_rel);
        } 
        else {
            archive_entry_free(entry);
        }
    }
    closedir(d);
}

int create_tar_gz(const char *archive_path, const char *src_dir) {
    struct archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, archive_path) != ARCHIVE_OK) {
        archive_write_free(a); return -1;
    }
    add_to_archive_recursive(a, src_dir, "");
    archive_write_close(a); archive_write_free(a);
    return 0;
}

void native_unload_sequence(const char *storage_base, const char *current_project, const char *active_dir, const char *state_file) {
    char tar_vault[PATH_SIZE];
    snprintf(tar_vault, sizeof(tar_vault), "%s/%s/project.tar.gz", storage_base, current_project);
    printf("[System] Archiving workspace additions into: %s/project.tar.gz\n", current_project);
    create_tar_gz(tar_vault, active_dir);
    native_rmdir_recursive(active_dir,0);
    unlink(state_file);
}

// Compares file sizes to detect active workspace modifications natively
void check_workspace_diff(const char *storage_base, const char *current_project, const char *active_dir, ExcludeList *excludes, int exclude_count, int *has_changes) {
    char truth_base_path[PATH_SIZE];
    snprintf(truth_base_path, sizeof(truth_base_path), "%s/%s/truth", storage_base, current_project);

    DIR *d = opendir(active_dir);
    if (!d) return;

    // Read the tracking state to know the root workspace path for relative calculations
    char workspace_root[PATH_SIZE] = "/home/laffin/project";
    char *env_active = getenv("PM_ACTIVE_DIR");
    if (env_active) {
        strncpy(workspace_root, env_active, sizeof(workspace_root) - 1);
    }

    struct dirent *p;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;
        if (strcmp(p->d_name, "project.tar.gz") == 0) continue;

        char active_file[PATH_SIZE], truth_file[PATH_SIZE];
        join_path(active_file, sizeof(active_file), active_dir, p->d_name);

        // Build the matching truth path relative to the master truth base
        const char *rel_to_root = active_file;
        if (strncmp(active_file, workspace_root, strlen(workspace_root)) == 0) {
            rel_to_root = active_file + strlen(workspace_root);
            if (*rel_to_root == '/') rel_to_root++;
        }
        snprintf(truth_file, sizeof(truth_file), "%s/%s", truth_base_path, rel_to_root);

        // --- NEW IGNORE FILTER LOGIC ---
        int skip = 0;
        for (int i = 0; i < exclude_count; i++) {
            if (strcmp(rel_to_root, excludes[i].name) == 0) {
                skip = 1;
                break;
            }
        }
        if (skip) continue; // Skip to next directory entry completely
        // -------------------------------

        struct stat st_active, st_truth;
        if (stat(active_file, &st_active) == 0) {
            if (S_ISDIR(st_active.st_mode)) {
                // Recursively traverse deep directories
		check_workspace_diff(storage_base, current_project, active_file, excludes, exclude_count, has_changes);
            } 
            else if (S_ISREG(st_active.st_mode)) {
                if (stat(truth_file, &st_truth) != 0) {
                    printf("  [Modified]  %s (New File)\n", rel_to_root);
                    *has_changes = 1;
                } else if (st_active.st_size != st_truth.st_size) {
                    printf("  [Modified]  %s (Size Changed)\n", rel_to_root);
                    *has_changes = 1;
                }
            }
        }
    }
    closedir(d);
}

void archive_truth_backup(const char *storage_base, const char *project_name) {
    char truth_dir[PATH_SIZE];
    char backup_root[PATH_SIZE];
    char archive_path[PATH_SIZE];
    char timestamp[64];

    snprintf(truth_dir, sizeof(truth_dir), "%s/%s/truth", storage_base, project_name);
    snprintf(backup_root, sizeof(backup_root), "%s/%s/truth_backups", storage_base, project_name);
    
    // Create the master backup container directory if it doesn't exist
    mkdir(backup_root, 0755);

    // Build unique snapshot filename
    get_backup_timestamp(timestamp, sizeof(timestamp));
    snprintf(archive_path, sizeof(archive_path), "%s/truth_%s.tar.gz", backup_root, timestamp);

    // Initialize libarchive structures
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_add_filter_gzip(a);

    if (archive_write_open_filename(a, archive_path) == ARCHIVE_OK) {
        printf("[Backup] Compressing stable truth vault into truth_backups/truth_%s.tar.gz...\n", timestamp);
        // Pack the exact contents of the truth vault
        add_to_archive_recursive(a, truth_dir, "");
        printf("[Success] Master snapshot backup created safely.\n");
    } else {
        printf("[Error] Failed to open target backup archive path.\n");
    }

    archive_write_close(a);
    archive_write_free(a);
}

int extract_truth_backup(const char *storage_base, const char *project_name, const char *timestamp) {
    char truth_dir[PATH_SIZE];
    char archive_path[PATH_SIZE];
    snprintf(truth_dir, sizeof(truth_dir), "%s/%s/truth", storage_base, project_name);
    snprintf(archive_path, sizeof(archive_path), "%s/%s/truth_backups/truth_%s.tar.gz", storage_base, project_name, timestamp);

    // Verify target backup exists before destructive actions
    struct stat st;
    if (stat(archive_path, &st) != 0) {
        printf("[Error] Backup archive for timestamp '%s' not found.\n", timestamp);
        return 0;
    }

    // Wipe current truth directory to ensure a clean slate restore
    // Using a simple system call to safely drop the old directory layout
    char rm_cmd[PATH_SIZE * 2];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", truth_dir, truth_dir);
    if (system(rm_cmd) != 0) {
        printf("[Error] Failed to reset target truth vault layout.\n");
        return 0;
    }

    // Extract archive into the truth directory
    struct archive *a = archive_read_new();
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    
    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        printf("[Error] Failed to open backup archive stream.\n");
        archive_read_free(a);
        archive_write_free(ext);
        return 0;
    }

    // Change runtime working directory to the target truth vault path to extract files seamlessly
    char old_cwd[PATH_SIZE];
    getcwd(old_cwd, sizeof(old_cwd));
    chdir(truth_dir);

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        archive_write_header(ext, entry);
        if (archive_entry_size(entry) > 0) {
            const void *buff;
            size_t size;
            int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                archive_write_data_block(ext, buff, size, offset);
            }
        }
        archive_write_finish_entry(ext);
    }

    chdir(old_cwd); // Return back to initial running location safely
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return 1;
}

typedef int (*command_handler_t)(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file);

static int cmd_init(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)active_dir; (void)state_file;
    if (argc < 3) { printf("[Error] Usage: project init <ProjectName>\n"); return 1; }
    ProjectLayout layout;
    init_project_layout(storage_base, argv[2], &layout);
    mkdir(layout.project_root, 0755);
    mkdir(layout.truth_dir, 0755);
    mkdir(layout.backup_root, 0755);

    FILE *f_init = fopen(layout.config_path, "w");
    if (f_init) {
        fprintf(f_init, "%s\nproject.config\n", argv[2]);
        fclose(f_init);
    }

    printf("[Success] Project folder and stable vault layout initialized.\n");
    return 0;
}

static int cmd_list(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)argc; (void)argv; (void)active_dir; (void)state_file;
    DIR *d = opendir(storage_base);
    if (!d) { printf("[Error] Cannot open persistent storage vault folder.\n"); return 1; }
    printf("====================================================================\n");
    printf("                  PERSISTENT STORAGE PROJECTS LIST                  \n");
    printf("====================================================================\n");
    struct dirent *p; int count = 0;
    while ((p = readdir(d))) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;
        char full_project_path[PATH_SIZE];
        join_path(full_project_path, sizeof(full_project_path), storage_base, p->d_name);
        struct stat st;
        if (stat(full_project_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("  -> %s\n", p->d_name); count++;
        }
    }
    closedir(d);
    if (count == 0) { printf("  (No projects found in storage vault directory)\n"); }
    printf("====================================================================\n");
    return 0;
}

static int cmd_ignore(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)storage_base; (void)active_dir;
    if (argc < 3) { printf("[Error] Usage: project ignore <FileName/Pattern>\n"); return 1; }
    FILE *f = fopen(state_file, "r+");
    if (!f) { printf("[Error] No active project loaded. Load a project first.\n"); return 1; }

    char line[NAME_SIZE];
    char header_project_name[NAME_SIZE];
    int duplicate = 0;

    if (fscanf(f, "%255s", header_project_name) == 1) {
        while (fscanf(f, "%255s", line) == 1) {
            if (strcmp(line, argv[2]) == 0) { duplicate = 1; break; }
        }
    }
    if (!duplicate) {
        fseek(f, 0, SEEK_END);
        fprintf(f, "%s\n", argv[2]);
        printf("[Success] Added '%s' to active tracking block parameters.\n", argv[2]);
    } else {
        printf("[Info] '%s' is already registered in tracking block array.\n", argv[2]);
    }
    fclose(f);
    return 0;
}

static int cmd_status(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)argc; (void)argv;
    printf("====================================================================\n");
    printf("                  PROJECT MANAGEMENT RUNTIME STATUS                 \n");
    printf("====================================================================\n");
    FILE *f = fopen(state_file, "r");
    if (!f) {
        printf("  Active Environment State : IDLE\n");
        printf("  Current Loaded Workspace : None\n");
    } else {
        char current[NAME_SIZE];
        ExcludeList excludes[MAX_EXCLUDES];
        int exclude_count = 0;
        if (fscanf(f, "%255s", current) == 1) {
            ProjectLayout layout;
            init_project_layout(storage_base, current, &layout);
            migrate_project_layout(storage_base, current, &layout);
            while (exclude_count < MAX_EXCLUDES && fscanf(f, "%255s", excludes[exclude_count].name) == 1) {
                exclude_count++;
            }
            printf("  Active Environment State  : TRACKING\n");
            printf("  Current Loaded Workspace  : %s\n", current);
            printf("  Persistent Vault Path     : %s\n", layout.truth_dir);
            printf("  Transient Active Workspace: %s\n\n", active_dir);

            printf("--- Workspace Changes ---\n");
            int has_changes = 0;
            check_workspace_diff(storage_base, current, active_dir, excludes, exclude_count, &has_changes);
            if (has_changes) {
                printf("\n  [Reminder] You have uncommitted modifications in your workspace!\n");
                printf("             -> To track changes permanently in your stable vault:\n");
                printf("                project commit <filename>\n\n");
                printf("             -> To completely hide a file or binary from being tracked:\n");
                printf("                project ignore <filename>\n");
            } else {
                printf("  All workspace source files match your stable truth vault perfectly.\n");
            }
        }
        fclose(f);
    }
    printf("====================================================================\n");
    return 0;
}

static int cmd_load(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    if (argc < 3) { printf("[Error] Usage: project load <ProjectName>\n"); return 1; }
    FILE *check_f = fopen(state_file, "r");
    if (check_f) {
        char active_pname[NAME_SIZE];
        if (fscanf(check_f, "%255s", active_pname) == 1) {
            printf("\n[Attention] Project '%s' is already active!\n", active_pname);
            printf("Selection [u/i/c]: ");
            char choice = '\0';
            if (scanf(" %c", &choice) != 1) { choice = 'c'; }
            if (choice == 'c' || choice == 'C') { fclose(check_f); return 0; }
            else if (choice == 'u' || choice == 'U') {
                fclose(check_f);
                native_unload_sequence(storage_base, active_pname, active_dir, state_file);
            } else { fclose(check_f); }
        } else { fclose(check_f); }
    }

    native_rmdir_recursive(active_dir, 0);
    ProjectLayout layout;
    migrate_project_layout(storage_base, argv[2], &layout);
    native_copy_dir(layout.truth_dir, active_dir);
    if (path_is_file(layout.archive_path)) {
        extract_tar_gz(layout.archive_path, active_dir);
    }

    native_copy_file(layout.config_path, state_file);
    printf("[Success] Project '%s' loaded dynamically.\n", argv[2]);
    return 0;
}

static int cmd_unload(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)argc; (void)argv;
    FILE *f = fopen(state_file, "r");
    if (!f) { printf("[Error] No active project context detected to unload.\n"); return 1; }
    char current[NAME_SIZE], persistent_cfg[PATH_SIZE];
    if (fscanf(f, "%255s", current) != 1) { fclose(f); return 1; }
    fclose(f);

    ProjectLayout layout;
    migrate_project_layout(storage_base, current, &layout);
    native_copy_file(state_file, layout.config_path);

    native_unload_sequence(storage_base, current, active_dir, state_file);
    printf("[Success] Workspace clean-up complete.\n");
    return 0;
}

static int cmd_backup(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)active_dir;
    FILE *f = fopen(state_file, "r");
    if (!f) {
        printf("[Error] No active project loaded. Load a project first.\n");
        return 1;
    }
    char current_project[NAME_SIZE];
    if (fscanf(f, "%255s", current_project) != 1) {
        printf("[Error] Failed to read active workspace profile.\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    ProjectLayout layout;
    migrate_project_layout(storage_base, current_project, &layout);

    if (argc >= 3 && strcmp(argv[2], "list") == 0) {
        DIR *dir = opendir(layout.backup_root);
        if (!dir) {
            printf("[Info] No historical backups found for this project vault.\n");
            return 0;
        }
        printf("====================================================================\n");
        printf("               AVAILABLE TRUTH VAULT BACKUP HISTORY                 \n");
        printf("====================================================================\n");
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(dir))) {
            if (strstr(ent->d_name, "truth_") && strstr(ent->d_name, ".tar.gz")) {
                char ts[64];
                strncpy(ts, ent->d_name + 6, 15);
                ts[15] = '\0';
                printf("  -> Backup Timestamp ID: %s\n", ts);
                count++;
            }
        }
        if (count == 0) printf("  No compressed archive tracking baselines registered yet.\n");
        printf("====================================================================\n");
        closedir(dir);
        return 0;
    }

    if (argc >= 4 && strcmp(argv[2], "restore") == 0) {
        char response[10];
        printf("[Warning] Restoring will overwrite your stable master vault files entirely!\n");
        printf("Would you like to take a protective backup of your current state first? (y/n): ");
        if (fgets(response, sizeof(response), stdin)) {
            if (response[0] == 'y' || response[0] == 'Y') {
                archive_truth_backup(storage_base, current_project);
            }
        }

        if (extract_truth_backup(storage_base, current_project, argv[3])) {
            printf("[Success] Stable master truth vault rolled back to snapshot '%s'.\n", argv[3]);
        } else {
            printf("[Failure] Restore pipeline aborted.\n");
        }
        return 0;
    }

    archive_truth_backup(storage_base, current_project);
    return 0;
}

static int cmd_commit(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    if (argc < 3) { printf("[Error] Usage: project commit <FileName>\n"); return 1; }
    FILE *f = fopen(state_file, "r");
    if (!f) { printf("[Error] Action blocked. Load a project first.\n"); return 1; }
    char current[NAME_SIZE];
    if (fscanf(f, "%255s", current) != 1) { fclose(f); return 1; }
    fclose(f);

    ProjectLayout layout;
    migrate_project_layout(storage_base, current, &layout);

    char src_path[PATH_SIZE];
    char rel_path[PATH_SIZE];
    char dst_path[PATH_SIZE];

    if (realpath(argv[2], src_path) == NULL) {
        strncpy(src_path, argv[2], sizeof(src_path) - 1);
        src_path[sizeof(src_path) - 1] = '\0';
    }

    workspace_relative_path(active_dir, src_path, rel_path, sizeof(rel_path));
    snprintf(dst_path, sizeof(dst_path), "%s/%s", layout.truth_dir, rel_path);

    if (ensure_parent_directory(dst_path) != 0) {
        printf("[Error] Failed to create destination hierarchy for commit.\n");
        return 1;
    }

    if (native_copy_file(src_path, dst_path) == 0) {
        printf("[Success] Committed snapshot of '%s' into truth vault.\n", argv[2]);
        return 0;
    }

    printf("[Error] Commit operation failed.\n");
    return 1;
}

static int cmd_sync(int argc, char *argv[], const char *storage_base, const char *active_dir, const char *state_file) {
    (void)argc; (void)argv;
    FILE *f = fopen(state_file, "r");
    if (!f) { printf("[Error] Action blocked. No project open to sync.\n"); return 1; }
    char current[NAME_SIZE];
    if (fscanf(f, "%255s", current) != 1) { fclose(f); return 1; }
    fclose(f);

    ProjectLayout layout;
    migrate_project_layout(storage_base, current, &layout);
    printf("[Action] Resetting workspace source text from truth vault...\n");
    native_copy_dir(layout.truth_dir, active_dir);
    printf("[Success] Rollback applied safely.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("====================================================================\n");
        printf("                  PROJECT ENVIRONMENT UTILITY MANUAL                \n");
        printf("====================================================================\n");
        printf("Usage:\n");
        printf("  project init <name>   - Creates a persistent project vault on Windows.\n");
        printf("  project list          - Lists all available projects in your storage vault.\n");
        printf("  project status        - Displays runtime info and uncommitted changes.\n");
        printf("  project ignore <file> - Registers a binary or file to be skipped completely.\n");
        printf("  project load <name>   - Sets up active workspace from the vault.\n");
        printf("  project commit <file> - Moves a verified file into the truth vault.\n");
        printf("  project sync          - Discards active changes & resets to pristine vault.\n");
        printf("  project unload        - Zips active additions and clears the workspace.\n");
        printf("====================================================================\n");
        return 1;
    }

    char *storage_base = getenv("PM_STORAGE_DIR");
    char *active_dir = getenv("PM_ACTIVE_DIR");
    char state_file[PATH_SIZE];
    if (!storage_base || !active_dir) {
        printf("[Fatal Error] Environment variables (PM_STORAGE_DIR,PM_ACTIVE_DIR) missing.\n");
        return 1;
    }
    join_path(state_file, sizeof(state_file), active_dir, "project.config");

    static const struct {
        const char *name;
        command_handler_t handler;
    } commands[] = {
        {"init",    cmd_init},
        {"list",    cmd_list},
        {"ignore",  cmd_ignore},
        {"status",  cmd_status},
        {"load",    cmd_load},
        {"unload",  cmd_unload},
        {"backup",  cmd_backup},
        {"commit",  cmd_commit},
        {"sync",    cmd_sync}
    };

    char *command = argv[1];
    for (size_t i = 0; i < (sizeof(commands) / sizeof(commands[0])); ++i) {
        if (strcmp(command, commands[i].name) == 0) {
            return commands[i].handler(argc, argv, storage_base, active_dir, state_file);
        }
    }

    printf("[Error] Unknown instruction option sequence.\n");
    return 1;
}
