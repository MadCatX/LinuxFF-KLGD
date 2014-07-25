#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "klgd.h"

struct klgd_main_private {
	atomic_t can_send_commands;
	void *device_context;
	unsigned long earliest_update;
	struct klgd_command_stream *last_stream;
	size_t plugin_count;
	struct klgd_plugin **plugins;
	atomic_t send_asap;
	spinlock_t stream_lock;
	struct timer_list timer;

	int (*send_command_stream)(void *dev_ctx, struct klgd_command_stream *stream);
};

void klgd_free_stream(struct klgd_command_stream *s);
void klgd_timer_fired(unsigned long ctx);
void klgd_schedule_update(struct klgd_main *ctx);

bool klgd_append_stream(struct klgd_command_stream *target, struct klgd_command_stream *source)
{
	struct klgd_command **temp;
	size_t idx;

	if (!source->count)
		return true;

	temp = krealloc(target->commands, sizeof(struct klgd_command *) * (target->count + source->count), GFP_ATOMIC);
	if (!temp)
		return false;

	target->commands = temp;
	for (idx = 0; idx < source->count; idx++)
		target->commands[idx + target->count] = source->commands[idx];
	target->count += source->count;

	return true;
}

void klgd_build_command_stream(struct klgd_main *ctx)
{
	struct klgd_main_private *priv = ctx->private;
	const unsigned long now = jiffies;
	size_t idx;

	struct klgd_command_stream *s = kzalloc(sizeof(struct klgd_command_stream), GFP_ATOMIC);
	if (!s)
		return; /* FIXME: Try to do an update later when some memory might be available */

	for (idx = 0; idx < priv->plugin_count; idx++) {
 		struct klgd_plugin *plugin = priv->plugins[idx];
		struct klgd_command_stream *ss = plugin->get_commands(plugin, now);
		/* FIXME: Same as above */
		if (!klgd_append_stream(s, ss)) {
			klgd_free_stream(s);
			return;
		}
	}

	if (s->count) {
		atomic_set(&priv->can_send_commands, 0);
		priv->last_stream = s;
		priv->send_command_stream(priv->device_context, s);
	}
}

void klgd_free_stream(struct klgd_command_stream *s)
{
	size_t idx;

	if (!s)
		return;

	for (idx = 0; idx < s->count; idx++) {
		kfree(s->commands[idx]->bytes);
		kfree(s->commands[idx]);
	}
}

void klgd_deinit(struct klgd_main *ctx)
{
	struct klgd_main_private *priv = ctx->private;
	size_t idx;

	for (idx = 0; idx < priv->plugin_count; idx++) {
		struct klgd_plugin *plugin = priv->plugins[idx];

		plugin->deinit(plugin);
		kfree(plugin);
	}
	kfree(priv->plugins);

	kfree(priv);
	kfree(ctx);
}

int klgd_init(struct klgd_main *ctx, void *dev_ctx, int (*callback)(void *, struct klgd_command_stream *), const size_t plugin_count)
{
	struct klgd_main_private *priv = ctx->private;
	int ret;

	if (!ctx)
		return -EINVAL;
	if (plugin_count < 1)
		return -EINVAL;
	if (!callback)
		return -EINVAL;

	priv = kzalloc(sizeof(struct klgd_main_private), GFP_KERNEL);
	if (!ctx->private)
		return -ENOMEM;

	priv->plugins = kzalloc(sizeof(struct klgd_plugin *) * plugin_count, GFP_KERNEL);
	if (!ctx->private->plugins) {
		ret = -ENOMEM;
		goto err_out;
	}
	priv->plugin_count = plugin_count;

	atomic_set(&priv->can_send_commands, 1);
	priv->device_context = dev_ctx;
	priv->last_stream = NULL;
	spin_lock_init(&priv->stream_lock);
	priv->send_command_stream = callback;
	atomic_set(&priv->send_asap, 0);

	setup_timer(&priv->timer, klgd_timer_fired, (unsigned long)ctx);

	return 0;

err_out:
	kfree(ctx->private);

	return ret;
}

void klgd_notify_commands_sent(struct klgd_main *ctx)
{
	struct klgd_main_private *priv = ctx->private;

	kfree(priv->last_stream);

	if (atomic_read(&priv->send_asap)) {
		unsigned long flags;

		pr_debug("Command stream processed, send a new one immediately\n");

		spin_lock_irqsave(&priv->stream_lock, flags);
		klgd_build_command_stream(ctx);
		spin_unlock_irqrestore(&priv->stream_lock, flags);
		atomic_set(&priv->send_asap, 0);
	} else {
		pr_debug("Command stream processed, wait for timer\n");
		atomic_set(&priv->can_send_commands, 1);
	}
}

int klgd_post_event(struct klgd_main *ctx, size_t idx, void *data)
{
  struct klgd_plugin *plugin = ctx->private->plugins[idx];
  int ret;

  ret = plugin->post_event(plugin, data);
  if (ret)
    return ret;

  klgd_schedule_update(ctx);
  return 0;
}

int klgd_register_plugin(struct klgd_main *ctx, size_t idx, struct klgd_plugin *plugin)
{
	struct klgd_main_private *priv = ctx->private;

	if (priv->plugins[idx])
		return -EINVAL;

	priv->plugins[idx] = kzalloc(sizeof(struct klgd_plugin), GFP_KERNEL);
	if (!priv->plugins[idx])
		return -ENOMEM;

	priv->plugins[idx] = plugin;
	return 0;
}

void klgd_schedule_update(struct klgd_main *ctx)
{
	struct klgd_main_private *priv = ctx->private;
	const unsigned long now = jiffies;
	unsigned int events = 0;
	unsigned long earliest;
	size_t idx;

	for (idx = 0; idx < priv->plugin_count; idx++) {
		struct klgd_plugin *plugin = priv->plugins[idx];
		unsigned long t;

		if (plugin->get_update_time(plugin, now, &t)) {
			if (!events)
				earliest = t;
			else {
				if (time_before(t, earliest))
					earliest = t;
			}
			events++;
		}
	}

	if (!events) {
		pr_debug("No events, deactivating timer\n");
		del_timer(&priv->timer);
	} else {
		pr_debug("Events: %u, earliest: %lu, now: %lu\n", events, earliest, now);
		mod_timer(&priv->timer, earliest);
	}
}

void klgd_timer_fired(unsigned long ctx)
{
	struct klgd_main *m = (struct klgd_main *)ctx;
	struct klgd_main_private *priv = m->private;


	if (atomic_read(&priv->can_send_commands)) {
		unsigned long flags;

		pr_debug("Timer fired, can send commands now\n");

		spin_lock_irqsave(&priv->stream_lock, flags);
		klgd_build_command_stream(m);
		spin_unlock_irqrestore(&priv->stream_lock, flags);
	} else {
		pr_debug("Timer fired, last stream of commands is still being processed\n");
		atomic_set(&priv->send_asap, 1);
	}

	klgd_schedule_update(ctx);
}