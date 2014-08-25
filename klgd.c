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

#define TRYAGAIN_DELAY 5

struct klgd_main_private {
	struct delayed_work work;
	void *device_context;
	unsigned long earliest_update;
	struct klgd_command_stream *last_stream;
	size_t plugin_count;
	struct klgd_plugin **plugins;
	struct mutex plugins_lock;
	struct mutex send_lock;
	struct workqueue_struct *wq;

	int (*send_command_stream)(void *dev_ctx, const struct klgd_command_stream *stream);
};

static void klgd_free_stream(struct klgd_command_stream *s);
static void klgd_schedule_update(struct klgd_main_private *priv);

struct klgd_command * klgd_alloc_cmd(const size_t length)
{
	struct klgd_command *cmd = kzalloc(sizeof(struct klgd_command), GFP_KERNEL);
	char *bytes;
	if (!cmd)
		return NULL;

	/* Cast away the const-ness */
	bytes = kzalloc(sizeof(char) * length, GFP_KERNEL);
	if (!bytes) {
		kfree(cmd);
		return NULL;
	}
	*(char **)(&cmd->bytes) = bytes;
	cmd->length = length;
	return cmd;
}
EXPORT_SYMBOL_GPL(klgd_alloc_cmd);

struct klgd_command_stream * klgd_alloc_stream(void)
{
	return kzalloc(sizeof(struct klgd_command_stream), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(klgd_alloc_stream);	

bool klgd_append_cmd(struct klgd_command_stream *target, const struct klgd_command *cmd)
{
	const struct klgd_command **temp;

	if (!target) {
		printk(KERN_NOTICE "Cannot append to NULL stream\n");
		return false;
	}
	if (!cmd) {
		printk(KERN_NOTICE "Cannot append NULL cmd\n");
		return false;
	}

	temp = krealloc(target->commands, sizeof(struct klgd_command *) * (target->count + 1), GFP_KERNEL);
	if (!temp)
		return false;

	target->commands = temp;
	target->commands[target->count] = cmd;
	target->count++;

	return true;
}
EXPORT_SYMBOL_GPL(klgd_append_cmd);


static bool klgd_append_stream(struct klgd_command_stream *target, const struct klgd_command_stream *source)
{
	const struct klgd_command **temp;
	size_t idx;

	/* We got NULL command, skip it */
	if (!source)
		return true;
	/* We got command of zero length, skip it */
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
 * Called with plugins_lock held
 */
int klgd_build_command_stream(struct klgd_main_private *priv, struct klgd_command_stream **s)
{
	const unsigned long now = jiffies;
	size_t idx;

	*s = kzalloc(sizeof(struct klgd_command_stream), GFP_KERNEL);
	if (!s)
		return -EAGAIN;

	for (idx = 0; idx < priv->plugin_count; idx++) {
 		struct klgd_plugin *plugin = priv->plugins[idx];
		struct klgd_command_stream *ss = plugin->get_commands(plugin, now);
		if (!klgd_append_stream(*s, ss)) {
			klgd_free_stream(*s);
			return -EAGAIN;
		}
	}

	if ((*s)->count) {
		priv->last_stream = *s;
		printk(KERN_NOTICE "KLGD: Command stream built\n");
		return 0;
	}
	printk(KERN_NOTICE "KLGD: Command stream is empty\n");
	return -ENOENT;
}

static void klgd_delayed_work(struct work_struct *w)
{
	struct delayed_work *dw = container_of(w, struct delayed_work, work);
	struct klgd_main_private *priv = container_of(dw, struct klgd_main_private, work);
	struct klgd_command_stream *s;
	unsigned long now;
	int ret;

	mutex_lock(&priv->send_lock);

	printk(KERN_NOTICE "KLGD/WQ: Timer fired and send_lock acquired\n");

	mutex_lock(&priv->plugins_lock);
	printk(KERN_NOTICE "KLGD/WQ: Plugins state locked\n");
	ret = klgd_build_command_stream(priv, &s);
	mutex_unlock(&priv->plugins_lock);

	switch (ret) {
	case -EAGAIN:
		/* Unable to build command stream right now, try again */
		mutex_unlock(&priv->send_lock);
		queue_delayed_work(priv->wq, &priv->work, TRYAGAIN_DELAY);
		return;
	case -ENOENT:
		/* Empty command stream. Plugins have no work for us, exit */
		goto out;
		return;
	case 0:
		break;
	default:
		/* TODO: Handle unspecified error */
		break;
	}

	now = jiffies;	
	ret = priv->send_command_stream(priv->device_context, s);
	if (ret) {
		/* TODO: Error handling */
		printk(KERN_NOTICE "KLGD/WQ: Unable to send command stream, ret code %d\n", ret);
	} else
		printk(KERN_NOTICE "KLGD/WQ: Commands sent, time elapsed %u [msec]\n", jiffies_to_msecs(jiffies - now));
	kfree(s);

out:
	mutex_unlock(&priv->send_lock);

	/* We're done submitting, check if there is some work for us in the future */
	mutex_lock(&priv->plugins_lock);
	klgd_schedule_update(priv);
	printk(KERN_NOTICE "KLGD/WQ: Plugins state unlocked\n");
	mutex_unlock(&priv->plugins_lock);
}

static void klgd_free_command(const struct klgd_command *cmd)
{
	if (cmd) {
		kfree(cmd->bytes);
		kfree(cmd);
	}
}

static void klgd_free_stream(struct klgd_command_stream *s)
{
	size_t idx;

	if (!s)
		return;

	for (idx = 0; idx < s->count; idx++)
		klgd_free_command(s->commands[idx]);
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
EXPORT_SYMBOL_GPL(klgd_deinit);

int klgd_init(struct klgd_main *ctx, void *dev_ctx, int (*callback)(void *, const struct klgd_command_stream *), const size_t plugin_count)
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

	mutex_init(&priv->plugins_lock);
	mutex_init(&priv->send_lock);
	priv->wq = create_singlethread_workqueue("klgd_processing_loop");
	INIT_DELAYED_WORK(&priv->work, klgd_delayed_work);

	priv->plugins = kzalloc(sizeof(struct klgd_plugin *) * plugin_count, GFP_KERNEL);
	if (!priv->plugins) {
		printk(KERN_ERR "No memory for KLGD plugins\n");
		ret = -ENOMEM;
		goto err_out;
	}
	priv->plugin_count = plugin_count;

	priv->device_context = dev_ctx;
	priv->last_stream = NULL;
	priv->send_command_stream = callback;

	ctx->private = priv;
	return 0;

err_out:
	destroy_workqueue(priv->wq);
	kfree(ctx->private);

	return ret;
}
EXPORT_SYMBOL_GPL(klgd_init);

void klgd_lock_plugins(struct mutex *lock)
{
	mutex_lock(lock);
	printk(KERN_DEBUG "KLGD: Plugins state locked\n");
}
EXPORT_SYMBOL_GPL(klgd_lock_plugins);

int klgd_register_plugin(struct klgd_main *ctx, size_t idx, struct klgd_plugin *plugin)
{
	struct klgd_main_private *priv = ctx->private;

	if (priv->plugins[idx])
		return -EINVAL;

	plugin->plugins_lock = &priv->plugins_lock;
	priv->plugins[idx] = plugin;
	if (plugin->init)
	      return plugin->init(plugin);

	return 0;
}
EXPORT_SYMBOL_GPL(klgd_register_plugin);

void klgd_unlock_plugins(struct mutex *lock)
{
	mutex_unlock(lock);
	printk(KERN_DEBUG "KLGD: Plugins state unlocked, NOT scheduled\n");
}
EXPORT_SYMBOL_GPL(klgd_unlock_plugins);


void klgd_unlock_plugins_sched(struct mutex *lock)
{
	struct klgd_main_private *priv = container_of(lock, struct klgd_main_private, plugins_lock);

	klgd_schedule_update(priv);
	mutex_unlock(lock);
	printk(KERN_DEBUG "KLGD: Plugins state unlocked, rescheduled\n");
}
EXPORT_SYMBOL_GPL(klgd_unlock_plugins_sched);

static void klgd_schedule_update(struct klgd_main_private *priv)
{
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
			else if (time_before(t, earliest))
				earliest = t;
			events++;
		}
	}

	if (!events) {
		printk(KERN_NOTICE "No events, deactivating timer\n");
		cancel_delayed_work(&priv->work);
	} else {
		printk(KERN_NOTICE "Events: %u, earliest: %lu, now: %lu\n", events, earliest, now);
		queue_delayed_work(priv->wq, &priv->work, earliest - now);
	}
}
