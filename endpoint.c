/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2014 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2014 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "domain.h"
#include "endpoint.h"
#include "policy.h"

/* endpoints are by default owned by the bus owner */
static char *kdbus_devnode_ep(struct device *dev, umode_t *mode,
			      kuid_t *uid, kgid_t *gid)
{
	struct kdbus_ep *ep = dev_get_drvdata(dev);

	if (mode)
		*mode = ep->mode;
	if (uid)
		*uid = ep->uid;
	if (gid)
		*gid = ep->gid;

	return NULL;
}

static void kdbus_dev_release(struct device *dev)
{
	kfree(dev);
}

static struct device_type kdbus_devtype_ep = {
	.name		= "ep",
	.release	= kdbus_dev_release,
	.devnode	= kdbus_devnode_ep,
};

struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep)
{
	kref_get(&ep->kref);
	return ep;
}

/**
 * kdbus_ep_disconnect() - disconnect an endpoint
 * @ep:			Endpoint
 */
void kdbus_ep_disconnect(struct kdbus_ep *ep)
{
	mutex_lock(&ep->lock);
	if (ep->disconnected) {
		mutex_unlock(&ep->lock);
		return;
	}
	ep->disconnected = true;
	mutex_unlock(&ep->lock);

	/* disconnect all connections to this endpoint */
	for (;;) {
		struct kdbus_conn *conn;

		mutex_lock(&ep->lock);
		conn = list_first_entry_or_null(&ep->conn_list,
						struct kdbus_conn,
						ep_entry);
		if (!conn) {
			mutex_unlock(&ep->lock);
			break;
		}

		/* take reference, release lock, disconnect without lock */
		kdbus_conn_ref(conn);
		mutex_unlock(&ep->lock);

		kdbus_conn_disconnect(conn, false);
		kdbus_conn_unref(conn);
	}

	/* disconnect from bus */
	mutex_lock(&ep->bus->lock);
	list_del(&ep->bus_entry);
	mutex_unlock(&ep->bus->lock);

	if (ep->dev) {
		device_unregister(ep->dev);
		ep->dev = NULL;
	}
	if (ep->minor > 0) {
		mutex_lock(&ep->bus->domain->lock);
		idr_remove(&ep->bus->domain->idr, ep->minor);
		mutex_unlock(&ep->bus->domain->lock);
		ep->minor = 0;
	}
}

static void __kdbus_ep_free(struct kref *kref)
{
	struct kdbus_ep *ep = container_of(kref, struct kdbus_ep, kref);

	BUG_ON(!ep->disconnected);
	BUG_ON(!list_empty(&ep->conn_list));

	kdbus_policy_db_clear(&ep->policy_db);
	kdbus_bus_unref(ep->bus);
	kdbus_domain_user_unref(ep->user);
	kfree(ep->name);
	kfree(ep);
}

struct kdbus_ep *kdbus_ep_unref(struct kdbus_ep *ep)
{
	if (!ep)
		return NULL;

	kref_put(&ep->kref, __kdbus_ep_free);
	return NULL;
}

static struct kdbus_ep *kdbus_ep_find(struct kdbus_bus *bus, const char *name)
{
	struct kdbus_ep *e, *ep = NULL;

	mutex_lock(&bus->lock);
	list_for_each_entry(e, &bus->ep_list, bus_entry) {
		if (strcmp(e->name, name) != 0)
			continue;

		ep = kdbus_ep_ref(e);
		break;
	}
	mutex_unlock(&bus->lock);

	return ep;
}

/**
 * kdbus_ep_new() - create a new endpoint
 * @bus:		The bus this endpoint will be created for
 * @name:		The name of the endpoint
 * @mode:		The access mode for the device node
 * @uid:		The uid of the device node
 * @gid:		The gid of the device node
 * @policy:		Whether or not the endpoint should have a policy db
 * @ep:			Pointer to a reference where the new endpoint is stored
 *
 * This function will create a new enpoint with the given
 * name and properties for a given bus.
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_ep_new(struct kdbus_bus *bus, const char *name,
		 umode_t mode, kuid_t uid, kgid_t gid,
		 bool policy, struct kdbus_ep **ep)
{
	struct kdbus_ep *e;
	int ret;

	e = kdbus_ep_find(bus, name);
	if (e) {
		kdbus_ep_unref(e);
		return -EEXIST;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	mutex_init(&e->lock);
	kref_init(&e->kref);
	INIT_LIST_HEAD(&e->conn_list);
	kdbus_policy_db_init(&e->policy_db);
	e->uid = uid;
	e->gid = gid;
	e->mode = mode;
	e->has_policy = policy;

	e->name = kstrdup(name, GFP_KERNEL);
	if (!e->name) {
		ret = -ENOMEM;
		goto exit_free;
	}

	mutex_lock(&bus->domain->lock);
	/* register minor in our endpoint map */
	ret = idr_alloc(&bus->domain->idr, e, 1, 0, GFP_KERNEL);
	if (ret < 0) {
		if (ret == -ENOSPC)
			ret = -EEXIST;
		mutex_unlock(&bus->domain->lock);
		goto exit_free_name;
	}
	e->minor = ret;
	mutex_unlock(&bus->domain->lock);

	/* register bus endpoint device */
	e->dev = kzalloc(sizeof(*e->dev), GFP_KERNEL);
	if (!e->dev) {
		ret = -ENOMEM;
		goto exit_idr;
	}

	dev_set_name(e->dev, "%s/%s/%s", bus->domain->devpath, bus->name, name);
	e->dev->bus = &kdbus_subsys;
	e->dev->type = &kdbus_devtype_ep;
	e->dev->devt = MKDEV(bus->domain->major, e->minor);
	dev_set_drvdata(e->dev, e);
	ret = device_register(e->dev);
	if (ret < 0) {
		put_device(e->dev);
		e->dev = NULL;
		goto exit_idr;
	}

	/* link into bus  */
	mutex_lock(&bus->lock);
	if (bus->disconnected) {
		mutex_unlock(&bus->lock);
		ret = -ESHUTDOWN;
		goto exit_dev_unregister;
	}
	e->id = ++bus->ep_seq_last;
	e->bus = kdbus_bus_ref(bus);
	list_add_tail(&e->bus_entry, &bus->ep_list);
	mutex_unlock(&bus->lock);

	if (ep)
		*ep = e;
	return 0;

exit_dev_unregister:
	device_unregister(e->dev);
exit_idr:
	mutex_lock(&bus->domain->lock);
	idr_remove(&bus->domain->idr, e->minor);
	mutex_unlock(&bus->domain->lock);
exit_free_name:
	kfree(e->name);
exit_free:
	kdbus_policy_db_clear(&e->policy_db);
	kfree(e);
	return ret;
}

/**
 * kdbus_ep_policy_set() - set policy for an endpoint
 * @ep:			The endpoint
 * @items:		The kdbus items containing policy information
 * @items_size:		The total length of the items
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_ep_policy_set(struct kdbus_ep *ep,
			const struct kdbus_item *items,
			size_t items_size)
{
	return kdbus_policy_set(&ep->policy_db, items, items_size, 0, true, ep);
}

/**
 * kdbus_ep_policy_check_see_access_unlocked() - verify a connection can see
 *						 the passed name
 * @ep:			Endpoint to operate on
 * @conn:		Connection that lists names
 * @name:		Name that is tried to be listed
 *
 * This verifies that @conn is allowed to see the well-known name @name via the
 * endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_see_access_unlocked(struct kdbus_ep *ep,
					      struct kdbus_conn *conn,
					      const char *name)
{
	int ret;

	/*
	 * Check policy, if the endpoint of the connection has a db.
	 * Note that policy DBs instanciated along with connections
	 * don't have SEE rules, so it's sufficient to check the
	 * endpoint's database.
	 *
	 * The lock for the policy db is held across all calls of
	 * kdbus_name_list_all(), so the entries in both writing
	 * and non-writing runs of kdbus_name_list_write() are the
	 * same.
	 */

	if (ep->has_policy) {
		ret = kdbus_policy_check_see_access_unlocked(&ep->policy_db,
							     conn, name);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/**
 * kdbus_ep_policy_check_talk_access() - verify a connection can talk to the
 *					 the passed connection
 * @ep:			Endpoint to operate on
 * @conn_src:		Connection that tries to talk
 * @conn_dst:		Connection that is talked to
 *
 * This verifies that @conn_src is allowed to talk to @conn_dst via the
 * endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_talk_access(struct kdbus_ep *ep,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst)
{
	int ret;

	if (ep->has_policy) {
		ret = kdbus_policy_check_talk_access(&ep->policy_db,
						     conn_src, conn_dst);
		if (ret < 0)
			return ret;
	}

	ret = kdbus_policy_check_talk_access(&ep->bus->policy_db,
					     conn_src, conn_dst);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * kdbus_ep_policy_check_own_access() - verify a connection can own the passed
 *					name
 * @ep:			Endpoint to operate on
 * @conn:		Connection that acquires a name
 * @name:		Name that is about to be acquired
 *
 * This verifies that @conn is allowed to acquire the well-known name @name via
 * the endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_own_access(struct kdbus_ep *ep,
				     const struct kdbus_conn *conn,
				     const char *name)
{
	int ret;

	if (ep->has_policy) {
		ret = kdbus_policy_check_own_access(&ep->policy_db, conn, name);
		if (ret < 0)
			return ret;
	}

	ret = kdbus_policy_check_own_access(&ep->bus->policy_db, conn, name);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * kdbus_ep_make_user() - create endpoint data from user data
 * @make:		The returned copy of user data
 * @name:		The name of the endpoint to create
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_ep_make_user(const struct kdbus_cmd_make *make, char **name)
{
	const struct kdbus_item *item;
	const char *n = NULL;
	int ret;

	KDBUS_ITEMS_FOREACH(item, make->items, KDBUS_ITEMS_SIZE(make, items)) {
		if (!KDBUS_ITEM_VALID(item, &make->items,
				      KDBUS_ITEMS_SIZE(make, items)))
			return -EINVAL;

		switch (item->type) {
		case KDBUS_ITEM_MAKE_NAME:
			if (n)
				return -EEXIST;

			ret = kdbus_item_validate_name(item);
			if (ret < 0)
				return ret;

			n = item->str;
			continue;
		}
	}

	if (!KDBUS_ITEMS_END(item, make->items, KDBUS_ITEMS_SIZE(make, items)))
		return -EINVAL;

	if (!n)
		return -EBADMSG;

	*name = (char *)n;
	return 0;
}
