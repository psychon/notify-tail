#include <errno.h>
#include <fcntl.h>
#include <libnotify/notify.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>

#define APPNAME "notify-tail"

#define LINE_BUFFER_SIZE 4096
#define TIMEOUT 10000

struct file_watch {
	struct file_watch *next;
	int watch_desc;

	struct file_watch *parent;

	const char *name;
	int fd;

	off_t file_offset;

	char line_buffer_data[LINE_BUFFER_SIZE];
	size_t line_buffer_pos;
};

static int inotify_fd;
static struct file_watch *file_watches = NULL;

static void handle_line(struct file_watch *watch, const char *line)
{
	NotifyNotification *notify;
	gchar *utf8;
	
	if (*line == '\0')
		return;

	utf8 = g_locale_to_utf8(line, -1, NULL, NULL, NULL);

	if (!utf8)
		utf8 = g_strdup_printf("ERROR: Read invalid line from '%s'",
				watch->name);

	notify = notify_notification_new(utf8, NULL, NULL);
	notify_notification_set_urgency(notify, NOTIFY_URGENCY_LOW);
	notify_notification_set_timeout(notify, TIMEOUT);
	notify_notification_show(notify, NULL);
	g_object_unref(G_OBJECT(notify));
	g_free(utf8);
}

static void
handle_lines(struct file_watch *watch)
{
	char *start = watch->line_buffer_data;
	char *end = strchr(start, '\n');

	while (end != NULL) {
		*end = '\0';
		handle_line(watch, start);

		start = end + 1;
		end = strchr(start, '\n');
	}

	if (start != watch->line_buffer_data) {
		/* This is where the data for the next line is in the buffer */
		size_t offset = start - watch->line_buffer_data;
		size_t pos = watch->line_buffer_pos;

		if (pos > offset)
			memmove(watch->line_buffer_data, start, pos - offset);
		watch->line_buffer_pos -= offset;
	}

	if (watch->line_buffer_pos >= LINE_BUFFER_SIZE - 1) {
		printf("'%s': Line longer than %d characters, splitting up\n",
				watch->name, LINE_BUFFER_SIZE);
		handle_line(watch, watch->line_buffer_data);
		watch->line_buffer_pos = 0;
	}
}

static bool
do_read(struct file_watch *watch)
{
	size_t pos = watch->line_buffer_pos;
	char *buf = &watch->line_buffer_data[pos];
	ssize_t ret = read(watch->fd, buf, LINE_BUFFER_SIZE - pos - 1);

	if (ret < 0) {
		fprintf(stderr, "Error while reading from '%s': %s\n",
				watch->name, strerror(ret));
		return false;
	}
	if (ret == 0)
		return false;

	watch->line_buffer_pos += ret;
	watch->line_buffer_data[watch->line_buffer_pos] = '\0';

	return true;
}

static void
read_watch(struct file_watch *watch)
{
	struct stat buf;

	fstat(watch->fd, &buf);
	if (buf.st_size < watch->file_offset) {
		printf("Warning: '%s' was truncated, reading whole file again\n",
				watch->name);
		lseek(watch->fd, 0, SEEK_SET);
	}

	while (do_read(watch))
		handle_lines(watch);

	watch->file_offset = lseek(watch->fd, 0, SEEK_CUR);
}

static struct file_watch *
find_watch(int wd)
{
	struct file_watch *watch = file_watches;

	while (watch != NULL && watch->watch_desc != wd)
		watch = watch->next;

	return watch;
}

static struct file_watch *
find_watch_by_name(const char *name)
{
	struct file_watch *watch = file_watches;

	while (watch != NULL) {
		if (watch->name && strcmp(watch->name, name) == 0)
			break;
		watch = watch->next;
	}

	return watch;
}

static struct file_watch *
init_watch(const char *name, bool is_file)
{
	struct file_watch *watch;

	watch = calloc(1, sizeof(*watch));
	watch->name = name;
	watch->fd = -1;

	if (file_watches == NULL) {
		file_watches = watch;
	} else {
		struct file_watch *pos = file_watches;

		while (pos->next != NULL)
			pos = pos->next;
		pos->next = watch;
	}

	if (!is_file)
		watch->watch_desc = inotify_add_watch(inotify_fd, watch->name, IN_CREATE | IN_MOVED_TO);
	else
		watch->watch_desc = -1;

	return watch;
}

static void
wait_for_parent(struct file_watch *watch)
{
	char *path;
	char *end;

	if (watch->parent)
		return;

	path = strdup(watch->name);
	end = strrchr(path, '/');
	if (end) {
		*end = '\0';
		watch->parent = find_watch_by_name(path);
		if (!watch->parent)
			watch->parent = init_watch(path, false);
		else
			free(path);
	} else
		free(path);
}

static void
file_deleted(struct file_watch *watch)
{
	inotify_rm_watch(inotify_fd, watch->watch_desc);
	close(watch->fd);
	watch->fd = -1;
	watch->watch_desc = -1;
	watch->file_offset = 0;
	watch->line_buffer_pos = 0;
}

static void
reinit_file(struct file_watch *watch)
{
	int fd;

	if (watch->fd >= 0)
		close(watch->fd);
	watch->fd = -1;
	if (watch->watch_desc >= 0)
		inotify_rm_watch(inotify_fd, watch->watch_desc);
	watch->watch_desc = -1;

	wait_for_parent(watch);

	fd = open(watch->name, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Could not open '%s': %s\n", watch->name, strerror(errno));
		return;
	}
	watch->fd = fd;
	
	watch->watch_desc = inotify_add_watch(inotify_fd, watch->name, IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
	if (watch->watch_desc >= 0) {
		watch->file_offset = lseek(watch->fd, 0, SEEK_END);
	} else {
		fprintf(stderr, "Failed to add watch for '%s': %s\n", watch->name, strerror(errno));
	}
}

static bool
init_file(const char *name)
{
	struct file_watch *watch;

	watch = init_watch(strdup(name), true);
	reinit_file(watch);
	return true;
}

static bool
init_files(int num, char * const names[])
{
	int i;

	for (i = 0; i < num; i++) {
		if (!init_file(names[i]))
			return false;
	}

	return true;
}

static void
file_appeared(struct file_watch *watch, const char *name)
{
	struct file_watch *file = file_watches;
	size_t dir_len = strlen(watch->name);

	while (file != NULL) {
		if (strncmp(watch->name, file->name, dir_len) == 0) {
			/* Ok, so this is the right dir, but is it also the
			 * right file? */
			const char *file_name = file->name + dir_len;
			while (file_name[0] == '/')
				file_name++;

			if (strcmp(file_name, name) == 0) {
				/* Yeah, this is the right watch, re-enable it */
				reinit_file(file);
			}
		}

		file = file->next;
	}
}

static void
handle_event(struct file_watch *watch, const char *name, uint32_t mask)
{
	if (mask & IN_MODIFY) {
		mask &= ~IN_MODIFY;
		read_watch(watch);
	}
	if (mask & IN_CREATE || mask & IN_MOVED_TO) {
		/* This is a watch for a directory, did someone create a file
		 * that we are looking for?
		 */
		mask &= ~(IN_CREATE | IN_MOVED_TO);
		file_appeared(watch, name);
	}
	if (mask & IN_DELETE_SELF || mask & IN_MOVE_SELF) {
		mask &= ~(IN_DELETE_SELF | IN_MOVE_SELF);
		file_deleted(watch);
	}
	/* The file was deleted or we removed the watch. Whatever. */
	mask &= ~IN_IGNORED;
	if (mask != 0) {
		fprintf(stderr, "Unhandled event 0x%08x for file '%s'\n", mask, watch->name);
	}
}

static void
read_events(void)
{
	const unsigned int event_buf_len = 4096;
	char buffer[event_buf_len];
	size_t pos = 0;
	size_t length;
	ssize_t ret;

	ret = read(inotify_fd, buffer, sizeof(buffer));
	if (ret < 0) {
		fprintf(stderr, "Error reading from inotify: %s\n", strerror(errno));
		return;
	}

	length = (size_t) ret;
	while (pos + sizeof(struct inotify_event) <= length) {
		struct inotify_event *event = (struct inotify_event *) &buffer[pos];
		struct file_watch *watch = find_watch(event->wd);
		if (watch)
			handle_event(watch, &event->name[0], event->mask);
		else
			fprintf(stderr, "Event %08x for unknown watch descriptor %d!\n",
					event->mask, event->wd);

		pos += sizeof(struct inotify_event) + event->len;
	}

	if (pos != length)
		fprintf(stderr, "inotify event of size %d, but handled %d bytes\n",
				(int) length, (int) pos);
}

int
main(int argc, char *argv[])
{
	notify_init(APPNAME);

	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		fprintf(stderr, "inotify not available\n");
		return -1;
	}

	if (!init_files(argc - 1, &argv[1])) {
		fprintf(stderr, "Startup failed\n");
		return -1;
	}

	do {
		read_events();
	} while (true);

	notify_uninit();
	close(inotify_fd);
	return 0;
}
