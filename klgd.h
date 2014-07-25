struct klgd_command {
	const char *bytes;
	size_t length;
};

struct klgd_command_stream {
	struct klgd_command **commands;
	size_t count;
};

struct klgd_main {
	struct klgd_main_private *private;
};

struct klgd_plugin {
	struct klgd_plugin_private *private;

	void (*deinit)(struct klgd_plugin *ctx, void *data);
	struct klgd_command_stream *(*get_commands)(struct klgd_plugin *ctx, const unsigned long now);
	bool (*get_update_time)(struct klgd_plugin *ctx, const unsigned long now, unsigned long *t);
	int (*init)(struct klgd_plugin *ctx, void *data);
	bool (*needs_attention)(struct klgd_plugin *ctx);
	int (*post_event)(struct klgd_plugin *ctx, void *data);
};

void klgd_deinit(struct klgd_main *ctx, void *data);
int klgd_init(struct klgd_main *ctx, void *dev_ctx, int (*callback)(void *, struct klgd_command_stream *), const unsigned long plugin_count);
int klgd_post_event(struct klgd_main *ctx, const size_t idx, void *data);
int klgd_register_plugin(struct klgd_main *ctx, const size_t idx, struct klgd_plugin *plugin);
