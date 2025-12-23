#include <ncurses.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

#define MAX_FILES 1024

char *playlist[MAX_FILES];
int file_count = 0;
int selected = 0;
int scroll_offset = 0;

void clear_playlist() {
    for (int i = 0; i < file_count; i++) free(playlist[i]);
    file_count = 0;
    selected = 0;
    scroll_offset = 0;
}

int is_midi(const char *name) {
    int len = strlen(name);
    return len > 4 && strcasecmp(name+len-4,".mid") == 0;
}

/* Case-insensitive strcmp wrapper for qsort */
static int ci_cmp(const void *a, const void *b) {
    const char *aa = *(const char * const *)a;
    const char *bb = *(const char * const *)b;
    return strcasecmp(aa, bb);
}

void load_playlist() {
    clear_playlist();
    DIR *d = opendir(".");
    if (!d) return;

    struct dirent *entry;

    /* temporary storage to separate dirs and files so we can sort them */
    char *dirs[MAX_FILES];
    int dirs_count = 0;
    char *files_arr[MAX_FILES];
    int files_count = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        int is_dir = 0;
#ifdef DT_DIR
        if (entry->d_type != DT_UNKNOWN) {
            if (entry->d_type == DT_DIR) is_dir = 1;
        } else {
            struct stat st;
            if (stat(entry->d_name, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
        }
#else
        {
            struct stat st;
            if (stat(entry->d_name, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
        }
#endif

        if (is_dir) {
            if (dirs_count < MAX_FILES) dirs[dirs_count++] = strdup(entry->d_name);
        } else if (is_midi(entry->d_name)) {
            if (files_count < MAX_FILES) files_arr[files_count++] = strdup(entry->d_name);
        }

        if (dirs_count + files_count >= MAX_FILES) break;
    }
    closedir(d);

    /* Sort directories and files separately, case-insensitive */
    if (dirs_count > 1) qsort(dirs, dirs_count, sizeof(char *), ci_cmp);
    if (files_count > 1) qsort(files_arr, files_count, sizeof(char *), ci_cmp);

    /* Build playlist: ".." first, then dirs, then files */
    if (file_count < MAX_FILES) playlist[file_count++] = strdup("..");
    for (int i = 0; i < dirs_count && file_count < MAX_FILES; i++) {
        playlist[file_count++] = dirs[i];
    }
    for (int i = 0; i < files_count && file_count < MAX_FILES; i++) {
        playlist[file_count++] = files_arr[i];
    }

    /* If we didn't append some entries due to MAX_FILES, free them */
    for (int i = dirs_count; i < dirs_count; i++) {} /* no-op to silence unused var pattern */
    /* Note: remaining slots in dirs/files arrays are either moved into playlist or should be freed.
       We moved ownership of strdup'd strings into playlist for the ones appended; any unappended
       strings should be freed. */
    /* Free any leftover (not appended) dir entries */
    for (int i = file_count - 1, j = dirs_count; j < dirs_count; ++j) {
        (void)i; /* no-op */
    }
    /* Free any leftover (not appended) file entries */
    /* (In this implementation we only allocated up to MAX_FILES and append in same order,
       so there shouldn't be leftovers unless MAX_FILES limit was hit; but to be safe:) */
    int used_from_dirs = (file_count > 0) ? ( (file_count - 1) < dirs_count ? (file_count - 1) : dirs_count ) : 0;
    for (int i = used_from_dirs; i < dirs_count; ++i) free(dirs[i]);
    int used_from_files = file_count - 1 - used_from_dirs;
    if (used_from_files < 0) used_from_files = 0;
    for (int i = used_from_files; i < files_count; ++i) free(files_arr[i]);
}

void draw_ui(WINDOW *left, WINDOW *right, int h, int w) {
    werase(left);
    box(left,0,0);
    mvwprintw(left,1,2,"Playlist");

    int max_visible = h - 5;
    if (selected < scroll_offset) scroll_offset = selected;
    if (selected >= scroll_offset + max_visible) scroll_offset = selected - max_visible + 1;

    int left_width = w/2 - 4;
    char name_buf[256];

    for (int i = 0; i < max_visible && (i+scroll_offset)<file_count; i++) {
        int idx = i + scroll_offset;
        const char *name = playlist[idx];

        if (idx == selected) wattron(left, A_REVERSE);

        if (strcmp(name,"..")==0) snprintf(name_buf,sizeof(name_buf),"[..]");
        else {
            struct stat st;
            if (stat(name,&st)==0 && S_ISDIR(st.st_mode))
                snprintf(name_buf,sizeof(name_buf), "[%s]", name);
            else strncpy(name_buf,name,sizeof(name_buf)-1);
        }
        name_buf[sizeof(name_buf)-1] = '\0';
        if ((int)strlen(name_buf) > left_width) name_buf[left_width] = '\0';

        mvwprintw(left, 2+i, 2, "%s", name_buf);

        if (idx == selected) wattroff(left, A_REVERSE);
    }
    wrefresh(left);

    werase(right);
    box(right,0,0);
    mvwprintw(right,1,2,"MIDI Info");
    int right_width = w/2 - 4;
    char info_buf[512];
    if (file_count>0) {
        const char *base = playlist[selected];
        if (strcmp(base,"..")!=0) snprintf(info_buf,sizeof(info_buf),"Selected: %s", base);
        else snprintf(info_buf,sizeof(info_buf),"Parent Folder");
        info_buf[right_width] = '\0';
        mvwprintw(right,3,2,"%s", info_buf);
    }
    wrefresh(right);

    mvprintw(h-1,0,"ENTER Enter Folder   Up/Down Navigate   PgUp/PgDn Scroll   SPACE Play/Pause   F10 Quit");
    clrtoeol();
    refresh();
}

void stop_midi() {
    system("killall aplaymidi >/dev/null 2>&1");
}

void play_selected() {
    if (file_count==0) return;
    const char *file = playlist[selected];
    struct stat st;
    if (strcmp(file,"..")==0) return;
    if (stat(file,&st)==0 && !S_ISDIR(st.st_mode)) {
        stop_midi();
        char cmd[1024];
        snprintf(cmd,sizeof(cmd),"aplaymidi \"%s\" >/dev/null 2>&1 &",file);
        system(cmd);
    }
}

int main(int argc,char *argv[]) {
    const char *start_dir = ".";
    if (argc>=2) start_dir = argv[1];
    if (chdir(start_dir)!=0) { perror("chdir"); return 1; }

    load_playlist();

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    int h,w;
    getmaxyx(stdscr,h,w);
    WINDOW *left = newwin(h-2,w/2,0,0);
    WINDOW *right = newwin(h-2,w/2,0,w/2);

    clear(); refresh();
    draw_ui(left,right,h,w);

    int ch;
    int max_visible = h-5;
    while ((ch=getch()) != KEY_F(10)) {
        switch(ch) {
            case KEY_UP: if (selected>0) selected--; break;
            case KEY_DOWN: if (selected<file_count-1) selected++; break;
            case KEY_PPAGE: selected-=max_visible; if(selected<0) selected=0; break;
            case KEY_NPAGE: selected+=max_visible; if(selected>file_count-1) selected=file_count-1; break;
            case 10: case KEY_ENTER:
                if (selected<file_count) {
                    const char *path = playlist[selected];
                    struct stat st;
                    if (strcmp(path,"..")==0) { chdir(".."); load_playlist(); selected=0; scroll_offset=0; }
                    else if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) { chdir(path); load_playlist(); selected=0; scroll_offset=0; }
                    else play_selected();
                }
                break;
            case ' ':
                play_selected();
                break;
        }
        draw_ui(left,right,h,w);
    }

    stop_midi();
    endwin();
    clear_playlist();
    return 0;
}
