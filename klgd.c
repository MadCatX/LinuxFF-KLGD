#include <asm/atomic.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include "klgd.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal \"MadCatX\" Maly");
MODULE_DESCRIPTION("Pluginable framework of helper functions to handle gaming devices");

struct klgd_main_private {
	struct delayed_work work;
	bool can_send_commands;
	void *device_context;
	unsigned long earliest_update;
	struct klgd_command_stream *last_stream;
	size_t plugin_count;
	struct klgd_plugin **plugins;
	unsigned int send_asap;
	struct mutex stream_mlock;
	struct workqueue_struct *wq;

	enum klgd_send_status (*send_command_stream)(void *dev_ctx, struct klgd_command_stream *stream);
};

static void klgd_free_stream(struct klgd_command_stream *s);
static void klgd_notify_commands_sent_internal(struct klgd_main_private *priv);
static void klgd_schedule_update(struct klgd_main *ctx);

static bool klgd_append_stream(struct klgd_command_stream *target, struct klgd_command_stream *source)
{
	struct klgd_command **temp;
	size_t idx;

	if (!source->count)
		return true;

	temp = krealloc(target->commands, sizeof(struct klgd_command *) * (target->count + source->count), GFP_KERNEL);
	if (!temp)
		return false;

	target->commands = temp;
	for (idx = 0; idx < source->count; idx++)
		target->commands[idx + target->count] = source->commands[idx];
	target->count += source->count;

	return true;
}

/**
 * Called with stream_mlock held
 */
static enum klgd_send_status klgd_build_command_stream(struct klgd_main_private *priv)
{
	const unsigned long now = jiffies;
	size_t idx;

	struct klgd_command_stream *s = kzalloc(sizeof(struct klgd_command_stream), GFP_KERNEL);
	if (!s)
		return KLGD_SS_DONE; /* FIXME: Try to do an update later when some memory might be available */

	for (idx = 0; idx < priv->plugin_count; idx++) {
 		struct klgd_plugin *plugin = priv->plugins[idx];
		struct klgd_command_stream *ss = plugin->get_commands(plugin, now);
		/* FIXME: Same as above */
		if (!klgd_append_stream(s, ss)) {
			klgd_free_stream(s);
			return KLGD_SS_DONE;
		}
	}

	if (s->count) {
		priv->can_send_commands = false;
		priv->last_stream = s;
		return priv->send_command_stream(priv->device_context, s);
	}
	return KLGD_SS_DONE;
}

static void klgd_delayed_work(struct work_struct *w)
{
	struct delayed_work *dw = container_of(w, struct delayed_work, work);
	struct klgd_main_private *priv = container_of(dw, struct klgd_main_private, work);
	struct klgd_main *m = container_of(&priv, struct klgd_main, private);

	mutex_lock(&priv->stream_mlock);
	if (priv->can_send_commands) {
		int ret;

		pr_debug("Timer fired, can send commands now\n");
		ret = klgd_build_command_stream(priv);
		if (ret == KLGD_SS_DONE)
			klgd_notify_commands_sent_internal(priv);
	} else {
		pr_debug("Timer fired, last stream of commands is still being processed\n");
		priv->send_asap++;
	}


	klgd_schedule_update(m);
	mutex_unlock(&priv->stream_mlock);
}

static void klgd_free_stream(struct klgd_command_stream *s)
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

	cancel_delayed_work(&priv->work);
	flush_workqueue(priv->wq);
	destroy_workqueue(priv->wq);
	printk(KERN_NOTICE "KLGD deinit, workqueue terminated\n");

	for (idx = 0; idx < priv->plugin_count; idx++) {
		struct klgd_plugin *plugin = priv->plugins[idx];

		if (!plugin)
			continue;

		if (plugin->deinit)
			plugin->deinit(plugin);
		kfree(plugin);
	}
	kfree(priv->plugins);

	kfree(priv);
}

int klgd_init(struct klgd_main *ctx, void *dev_ctx, enum klgd_send_status (*callback)(void *, struct klgd_command_stream *), const size_t plugin_count)
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
	if (!priv) {
		printk(KERN_ERR "No memory for KLGD data\n");
		return -ENOMEM;
	}

	mutex_init(&priv->stream_mlock);
	priv->wq = create_singlethread_workqueue("klgd_processing_loop");
	INIT_DELAYED_WORK(&priv->work, klgd_delayed_work);

	priv->plugins = kzalloc(sizeof(struct klgd_plugin *) * plugin_count, GFP_KERNEL);
	if (!priv->plugins) {
		printk(KERN_ERR "No memory for KLGD plugins\n");
		ret = -ENOMEM;
		goto err_out;
	}
	priv->plugin_count = plugin_count;

	priv->can_send_commands = true;
	priv->device_context = dev_ctx;
	priv->last_stream = NULL;
	priv->send_command_stream = callback;
	priv->send_asap = 0;

	ctx->private = priv;
	return 0;

err_out:
	destroy_workqueue(priv->wq);
	kfree(ctx->private);

	return ret;
}

void klgd_notify_commands_sent(struct klgd_main *ctx)
{
	struct klgd_main_private *priv = ctx->private;

	mutex_lock(&priv->stream_mlock);
	klgd_notify_commands_sent_internal(priv);		
	mutex_unlock(&priv->stream_mlock);
}

/**
 * Called with stream_lock held
 */
static void klgd_notify_commands_sent_internal(struct klgd_main_private *priv)
{
	kfree(priv->last_stream);

	if (priv->send_asap) {
		pr_debug("Command stream processed, send a new one immediately\n");
		klgd_build_command_stream(priv);
		priv->send_asap--;
	} else {
		pr_debug("Command stream processed, wait for timer\n");
		priv->can_send_commands = true;
	}
}

int klgd_post_event(struct klgd_main *ctx, size_t idx, void *data)
{
	struct klgd_plugin *plugin = ctx->private->plugins[idx];
	int ret;

	if (!plugin || idx >= ctx->private->plugin_count)
		return -EINVAL;

	mutex_lock(&ctx->private->stream_mlock);
	ret = plugin->post_event(plugin, data);
	if (ret) {
		mutex_unlock(&ctx->private->stream_mlock);
		return ret;
	}

	klgd_schedule_update(ctx);
	mutex_unlock(&ctx->private->stream_mlock);
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
	if (plugin->init)
	      plugin->init(plugin);

	return 0;
}

static void klgd_schedule_update(struct klgd_main *ctx)
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
		cancel_delayed_work(&priv->work);
	} else {
		pr_debug("Events: %u, earliest: %lu, now: %lu\n", events, earliest, now);
		queue_delayed_work(priv->wq, &priv->work, earliest - now);
	}
}

EXPORT_SYMBOL_GPL(klgd_deinit);
EXPORT_SYMBOL_GPL(klgd_init);
EXPORT_SYMBOL_GPL(klgd_notify_commands_sent);
EXPORT_SYMBOL_GPL(klgd_post_event);
EXPORT_SYMBOL_GPL(klgd_register_plugin);
