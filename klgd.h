enum klgd_send_status {
	KLGD_SS_RUNNING,
	KLGD_SS_DONE,
	KLGD_SS_FAILED,
	KLGD_SS_TRYAGAIN
};

struct klgd_command {
	char *bytes;
	size_t length;
};

struct klgd_command_stream {
	const struct klgd_command **commands;
	size_t count;
};

struct klgd_main {
	struct klgd_main_private *private;
};

struct klgd_plugin {
	struct klgd_plugin_private *private;
	struct mutex *plugins_lock;

	void (*deinit)(struct klgd_plugin *ctx);
	struct klgd_command_stream *(*get_commands)(struct klgd_plugin *ctx, const unsigned long now);
	bool (*get_update_time)(struct klgd_plugin *ctx, const unsigned long now, unsigned long *t);
	int (*init)(struct klgd_plugin *ctx);
};

struct klgd_command * klgd_alloc_cmd(const size_t length);
struct klgd_command_stream * klgd_alloc_stream(void);
bool klgd_append_cmd(struct klgd_command_stream *target, const struct klgd_command *cmd);
void klgd_deinit(struct klgd_main *ctx);
int klgd_init(struct klgd_main *ctx, void *dev_ctx, enum klgd_send_status (*callback)(void *, const struct klgd_command_stream *), const unsigned long plugin_count);
void klgd_lock_plugins(struct mutex *lock);
void klgd_notify_commands_sent(struct klgd_main *ctx);
int klgd_register_plugin(struct klgd_main *ctx, const size_t idx, struct klgd_plugin *plugin);
void klgd_unlock_plugins(struct mutex *lock);
void klgd_unlock_plugins_sched(struct mutex *lock);
