/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/*
 * Implementation notes
 * --------------------
 *
 * In handles, if task == NULL, no events will be generated, and no events
 * have been sent.  If task != NULL but taskaction == NULL, an event has been
 * posted but not yet freed.  If neigher are NULL, no event was posted.
 *
 */

#include <config.h>

#include <stdio.h>
#include <string.h>

#include <isc/assertions.h>
#include <isc/magic.h>
#include <isc/mutex.h>
#include <isc/mutexblock.h>
#include <isc/event.h>

#include <dns/address.h>
#include <dns/name.h>

#include "../isc/util.h"

#define DNS_ADB_MAGIC		  0x44616462	/* Dadb. */
#define DNS_ADB_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADB_MAGIC)
#define DNS_ADBNAME_MAGIC	  0x6164624e	/* adbN. */
#define DNS_ADBNAME_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBNAME_MAGIC)
#define DNS_ADBNAMEHOOK_MAGIC	  0x61644e48	/* adNH. */
#define DNS_ADBNAMEHOOK_VALID(x)  ISC_MAGIC_VALID(x, DNS_ADBNAMEHOOK_MAGIC)
#define DNS_ADBZONEINFO_MAGIC	  0x6164625a	/* adbZ. */
#define DNS_ADBZONEINFO_VALID(x)  ISC_MAGIC_VALID(x, DNS_ADBZONEINFO_MAGIC)
#define DNS_ADBENTRY_MAGIC	  0x61646245	/* adbE. */
#define DNS_ADBENTRY_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBENTRY_MAGIC)

/*
 * Lengths of lists needs to be powers of two.
 */
#define DNS_ADBNAMELIST_LENGTH	16	/* how many buckets for names */
#define DNS_ADBENTRYLIST_LENGTH	16	/* how many buckets for addresses */

#define FREE_ITEMS		16	/* free count for memory pools */
#define FILL_COUNT		 8	/* fill count for memory pools */

#define DNS_ADB_INVALIDBUCKET (-1)	/* invalid bucket address */

typedef struct dns_adbname dns_adbname_t;
typedef ISC_LIST(dns_adbname_t) dns_adbnamelist_t;
typedef struct dns_adbnamehook dns_adbnamehook_t;
typedef struct dns_adbzoneinfo dns_adbzoneinfo_t;
typedef ISC_LIST(dns_adbentry_t) dns_adbentrylist_t;

struct dns_adb {
	unsigned int			magic;

	isc_mutex_t			lock;
	isc_mem_t		       *mctx;

	isc_mutex_t			mplock;
	isc_mempool_t		       *nmp;	/* dns_adbname_t */
	isc_mempool_t		       *nhmp;	/* dns_adbnamehook_t */
	isc_mempool_t		       *zimp;	/* dns_adbzoneinfo_t */
	isc_mempool_t		       *emp;	/* dns_adbentry_t */
	isc_mempool_t		       *ahmp;	/* dns_adbhandle_t */
	isc_mempool_t		       *aimp;	/* dns_adbaddrinfo_t */

	/*
	 * Bucketized locks and lists for names.
	 */
	dns_adbnamelist_t		names[DNS_ADBNAMELIST_LENGTH];
	isc_mutex_t			namelocks[DNS_ADBNAMELIST_LENGTH];

	/*
	 * Bucketized locks for entries.
	 */
	dns_adbentrylist_t		entries[DNS_ADBENTRYLIST_LENGTH];
	isc_mutex_t			entrylocks[DNS_ADBENTRYLIST_LENGTH];

	/*
	 * List of running and idle handles.
	 */
	ISC_LIST(dns_adbhandle_t)	running_handles;
	ISC_LIST(dns_adbhandle_t)	idle_handles;
};

struct dns_adbname {
	unsigned int			magic;
	dns_name_t			name;
	ISC_LIST(dns_adbnamehook_t)	namehooks;
	ISC_LINK(dns_adbname_t)		link;
};

/*
 * dns_adbnamehook_t
 *
 * This is a small widget that dangles off a dns_adbname_t.  It contains a
 * pointer to the address information about this host, and a link to the next
 * namehook that will contain the next address this host has.
 */
struct dns_adbnamehook {
	unsigned int			magic;
	dns_adbentry_t		       *entry;
	ISC_LINK(dns_adbnamehook_t)	link;
};

/*
 * dns_adbzoneinfo_t
 *
 * This is a small widget that holds zone-specific information about an
 * address.  Currently limited to lameness, but could just as easily be
 * extended to other types of information about zones.
 */
struct dns_adbzoneinfo {
	unsigned int			magic;

	dns_name_t		       *zone;
	unsigned int			lame_timer;

	ISC_LINK(dns_adbzoneinfo_t)	link;
};

/*
 * An address entry.  It holds quite a bit of information about addresses,
 * including edns state, rtt, and of course the address of the host.
 */
struct dns_adbentry {
	unsigned int			magic;

	int				lock_bucket;
	unsigned int			refcount;

	unsigned int			flags;
	int				goodness;	/* bad <= 0 < good */
	unsigned int			srtt;
	isc_sockaddr_t			sockaddr;

	ISC_LIST(dns_adbzoneinfo_t)	zoneinfo;
	ISC_LINK(dns_adbentry_t)	link;
};

/*
 * Internal functions (and prototypes).
 */
static dns_adbname_t *new_adbname(dns_adb_t *);
static dns_adbnamehook_t *new_adbnamehook(dns_adb_t *, dns_adbentry_t *);
static dns_adbzoneinfo_t *new_adbzoneinfo(dns_adb_t *);
static dns_adbentry_t *new_adbentry(dns_adb_t *);
static dns_adbhandle_t *new_adbhandle(dns_adb_t *);
static dns_adbaddrinfo_t *new_adbaddrinfo(dns_adb_t *, dns_adbentry_t *);
static dns_adbname_t *find_name_and_lock(dns_adb_t *, dns_name_t *, int *);
static dns_adbentry_t *find_entry_and_lock(dns_adb_t *, isc_sockaddr_t *,
					   int *);
static void print_dns_name(FILE *, dns_name_t *);
static void print_namehook_list(FILE *, dns_adbname_t *);

static dns_adbname_t *
new_adbname(dns_adb_t *adb)
{
	dns_adbname_t *name;

	name = isc_mempool_get(adb->nmp);
	if (name == NULL)
		return (NULL);

	name->magic = DNS_ADBNAME_MAGIC;
	dns_name_init(&name->name, NULL);
	ISC_LIST_INIT(name->namehooks);
	ISC_LINK_INIT(name, link);

	return (name);
}

static void
free_adbname(dns_adb_t *adb, dns_adbname_t **name)
{
	dns_adbname_t *n;

	INSIST(name != NULL && DNS_ADBNAME_VALID(*name));
	n = *name;
	*name = NULL;

	INSIST(ISC_LIST_EMPTY(n->namehooks));
	INSIST(!ISC_LINK_LINKED(n, link));

	n->magic = 0;
	dns_name_free(&n->name, adb->mctx);

	isc_mempool_put(adb->nmp, n);
}

static dns_adbnamehook_t *
new_adbnamehook(dns_adb_t *adb, dns_adbentry_t *entry)
{
	dns_adbnamehook_t *nh;

	nh = isc_mempool_get(adb->nhmp);
	if (nh == NULL)
		return (NULL);

	nh->magic = DNS_ADBNAMEHOOK_MAGIC;
	nh->entry = entry;
	ISC_LINK_INIT(nh, link);

	return (nh);
}

static void
free_adbnamehook(dns_adb_t *adb, dns_adbnamehook_t **namehook)
{
	dns_adbnamehook_t *nh;

	INSIST(namehook != NULL && DNS_ADBNAMEHOOK_VALID(*namehook));
	nh = *namehook;
	*namehook = NULL;

	INSIST(nh->entry == NULL);
	INSIST(!ISC_LINK_LINKED(nh, link));

	nh->magic = 0;
	isc_mempool_put(adb->nhmp, nh);
}

static dns_adbzoneinfo_t *
new_adbzoneinfo(dns_adb_t *adb)
{
	dns_adbzoneinfo_t *zi;

	zi = isc_mempool_get(adb->zimp);
	if (zi == NULL)
		return (NULL);

	zi->magic = DNS_ADBZONEINFO_MAGIC;
	zi->zone = NULL;
	zi->lame_timer = 0;
	ISC_LINK_INIT(zi, link);

	return (zi);
}

static dns_adbentry_t *
new_adbentry(dns_adb_t *adb)
{
	dns_adbentry_t *e;

	e = isc_mempool_get(adb->emp);
	if (e == NULL)
		return (NULL);

	e->magic = DNS_ADBENTRY_MAGIC;
	e->lock_bucket = DNS_ADB_INVALIDBUCKET;
	e->refcount = 0;
	e->flags = 0;
	e->goodness = 0;
	e->srtt = 0;
	ISC_LIST_INIT(e->zoneinfo);
	ISC_LINK_INIT(e, link);

	return (e);
}

static void
free_adbentry(dns_adb_t *adb, dns_adbentry_t **entry)
{
	dns_adbentry_t *e;
	INSIST(entry != NULL && DNS_ADBENTRY_VALID(*entry));
	e = *entry;
	*entry = NULL;

	INSIST(e->lock_bucket == DNS_ADB_INVALIDBUCKET);
	INSIST(e->refcount == 0);
	INSIST(ISC_LIST_EMPTY(e->zoneinfo));
	INSIST(!ISC_LINK_LINKED(e, link));

	e->magic = 0;
	isc_mempool_put(adb->emp, e);
}

static dns_adbhandle_t *
new_adbhandle(dns_adb_t *adb)
{
	dns_adbhandle_t *h;
	isc_result_t result;

	h = isc_mempool_get(adb->ahmp);
	if (h == NULL)
		return (NULL);

	/*
	 * public members
	 */
	h->magic = 0;
	h->adb = adb;
	ISC_LIST_INIT(h->list);

	/*
	 * private members
	 */
	result = isc_mutex_init(&h->lock);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init failed in new_adbhandle()");
		isc_mempool_put(adb->ahmp, h);
		return (NULL);
	}
	h->task = NULL;
	h->taskaction = NULL;
	h->arg = NULL;
	ISC_LINK_INIT(h, link);

	ISC_EVENT_INIT(&h->event, sizeof (isc_event_t), 0, 0, 0, NULL, NULL,
		       NULL, NULL, h);

	h->magic = DNS_ADBHANDLE_MAGIC;
	return (h);
}

/*
 * Copy bits from the entry into the newly allocated addrinfo.  The entry
 * must be locked, and the reference count must be bumped up by one
 * if this function returns a valid pointer.
 */
static dns_adbaddrinfo_t *
new_adbaddrinfo(dns_adb_t *adb, dns_adbentry_t *entry)
{
	dns_adbaddrinfo_t *ai;

	ai = isc_mempool_get(adb->aimp);
	if (ai == NULL)
		return (NULL);

	ai->magic = DNS_ADBADDRINFO_MAGIC;
	ai->sockaddr = &entry->sockaddr;
	ai->goodness = entry->goodness;
	ai->srtt = entry->srtt;
	ai->flags = entry->flags;
	ai->entry = entry;
	ISC_LINK_INIT(ai, link);

	return (ai);
}

/*
 * Search for the name.  NOTE:  The bucket is kept locked on both
 * success and failure, so it must always be unlocked by the caller!
 *
 * On the first call to this function, *bucketp must be set to
 * DNS_ADB_INVALIDBUCKET.
 */
static dns_adbname_t *
find_name_and_lock(dns_adb_t *adb, dns_name_t *name, int *bucketp)
{
	dns_adbname_t *adbname;
	unsigned int bucket;

	bucket = dns_name_hash(name, ISC_FALSE);
	bucket &= (DNS_ADBNAMELIST_LENGTH - 1);

	LOCK(&adb->namelocks[bucket]);
	*bucketp = (int)bucket;

	adbname = ISC_LIST_HEAD(adb->names[bucket]);
	while (adbname != NULL) {
		if (dns_name_equal(name, &adbname->name))
			return (adbname);
		adbname = ISC_LIST_NEXT(adbname, link);
	}

	return (NULL);
}

/*
 * Search for the address.  NOTE:  The bucket is kept locked on both
 * success and failure, so it must always be unlocked by the caller.
 *
 * On the first call to this function, *bucketp must be set to
 * DNS_ADB_INVALIDBUCKET.  This will cause a lock to occur.  On
 * later calls (within the same "lock path") it can be left alone, so
 * if this function is called multiple times locking is only done if
 * the bucket changes.
 */
static dns_adbentry_t *
find_entry_and_lock(dns_adb_t *adb, isc_sockaddr_t *addr, int *bucketp)
{
	dns_adbentry_t *entry;
	int bucket;

	bucket = isc_sockaddr_hash(addr, ISC_TRUE);
	bucket &= (DNS_ADBENTRYLIST_LENGTH - 1);

	if (*bucketp == DNS_ADB_INVALIDBUCKET) {
		LOCK(&adb->entrylocks[bucket]);
		*bucketp = bucket;
	} else if (*bucketp != bucket) {
		UNLOCK(&adb->entrylocks[*bucketp]);
		LOCK(&adb->entrylocks[bucket]);
		*bucketp = bucket;
	}

	entry = ISC_LIST_HEAD(adb->entries[bucket]);
	while (entry != NULL) {
		if (isc_sockaddr_equal(addr, &entry->sockaddr))
			return (entry);
		entry = ISC_LIST_NEXT(entry, link);
	}

	return (NULL);
}

static void
destroy(dns_adb_t *adb)
{
	adb->magic = 0;

	isc_mempool_destroy(&adb->nmp);
	isc_mempool_destroy(&adb->nhmp);
	isc_mempool_destroy(&adb->zimp);
	isc_mempool_destroy(&adb->emp);
	isc_mempool_destroy(&adb->ahmp);
	isc_mempool_destroy(&adb->aimp);

	isc_mutexblock_destroy(adb->entrylocks, DNS_ADBENTRYLIST_LENGTH);
	isc_mutexblock_destroy(adb->namelocks, DNS_ADBNAMELIST_LENGTH);

	isc_mutex_destroy(&adb->lock);
	isc_mutex_destroy(&adb->mplock);

	isc_mem_put(adb->mctx, adb, sizeof (dns_adb_t));
}


/*
 * Public functions.
 */

isc_result_t
dns_adb_create(isc_mem_t *mem, dns_adb_t **newadb)
{
	dns_adb_t *adb;
	isc_result_t result;
	int i;

	REQUIRE(mem != NULL);
	REQUIRE(newadb != NULL && *newadb == NULL);

	adb = isc_mem_get(mem, sizeof (dns_adb_t));
	if (adb == NULL)
		return (ISC_R_NOMEMORY);

	/*
	 * Initialize things here that cannot fail, and especially things
	 * that must be NULL for the error return to work properly.
	 */
	adb->magic = 0;
	adb->nmp = NULL;
	adb->nhmp = NULL;
	adb->zimp = NULL;
	adb->emp = NULL;
	adb->ahmp = NULL;
	adb->aimp = NULL;

	result = isc_mutex_init(&adb->lock);
	if (result != ISC_R_SUCCESS)
		goto fail1;
	result = isc_mutex_init(&adb->mplock);
	if (result != ISC_R_SUCCESS)
		goto fail1;

	/*
	 * Initialize the bucket locks for names and elements.
	 * May as well initialize the list heads, too.
	 */
	result = isc_mutexblock_init(adb->namelocks, DNS_ADBNAMELIST_LENGTH);
	if (result != ISC_R_SUCCESS)
		goto fail1;
	for (i = 0 ; i < DNS_ADBNAMELIST_LENGTH ; i++)
		ISC_LIST_INIT(adb->names[i]);
	for (i = 0 ; i < DNS_ADBENTRYLIST_LENGTH ; i++)
		ISC_LIST_INIT(adb->entries[i]);
	result = isc_mutexblock_init(adb->entrylocks, DNS_ADBENTRYLIST_LENGTH);
	if (result != ISC_R_SUCCESS)
		goto fail2;

	/*
	 * Memory pools
	 */
#define MPINIT(t, p) do { \
	result = isc_mempool_create(mem, sizeof (t), &(p)); \
	if (result != ISC_R_SUCCESS) \
		goto fail3; \
	isc_mempool_setfreemax((p), FREE_ITEMS); \
	isc_mempool_setfillcount((p), FILL_COUNT); \
} while (0)
#define MPINIT_LOCKED(t, p) do { \
	MPINIT(t, p); \
	isc_mempool_associatelock((p), &adb->mplock); \
} while (0)

	MPINIT_LOCKED(dns_adbname_t,		adb->nmp);
	MPINIT_LOCKED(dns_adbnamehook_t,	adb->nhmp);
	MPINIT_LOCKED(dns_adbzoneinfo_t,	adb->zimp);
	MPINIT_LOCKED(dns_adbentry_t,		adb->emp);
	MPINIT_LOCKED(dns_adbhandle_t,		adb->ahmp);
	MPINIT_LOCKED(dns_adbaddrinfo_t,	adb->aimp);

#undef MPINIT
#undef MPINIT_LOCKED

	/*
	 * Normal return.
	 */
	adb->mctx = mem;
	adb->magic = DNS_ADB_MAGIC;
	*newadb = adb;
	return (ISC_R_SUCCESS);

 fail3: /* clean up entrylocks */
	isc_mutexblock_destroy(adb->entrylocks, DNS_ADBENTRYLIST_LENGTH);

 fail2: /* clean up namelocks */
	isc_mutexblock_destroy(adb->namelocks, DNS_ADBNAMELIST_LENGTH);

 fail1: /* clean up only allocated memory */
	if (adb->nmp != NULL)
		isc_mempool_destroy(&adb->nmp);
	if (adb->nhmp != NULL)
		isc_mempool_destroy(&adb->nhmp);
	if (adb->zimp != NULL)
		isc_mempool_destroy(&adb->zimp);
	if (adb->emp != NULL)
		isc_mempool_destroy(&adb->emp);
	if (adb->ahmp != NULL)
		isc_mempool_destroy(&adb->ahmp);
	if (adb->aimp != NULL)
		isc_mempool_destroy(&adb->aimp);

	isc_mutex_destroy(&adb->lock);
	isc_mutex_destroy(&adb->mplock);

	isc_mem_put(mem, adb, sizeof (dns_adb_t));

	return (result);
}

void
dns_adb_destroy(dns_adb_t **adbx)
{
	dns_adb_t *adb;

	REQUIRE(adbx != NULL && DNS_ADB_VALID(*adbx));

	adb = *adbx;
	*adbx = NULL;

	/*
	 * XXX Need to wait here until the adb is fully shut down.
	 */

	/*
	 * If all lists are empty, destroy the memory used by this
	 * adb.  XXX Need to implement this.
	 */

	destroy(adb);
}

isc_result_t
dns_adb_lookup(dns_adb_t *adb, isc_task_t *task, isc_taskaction_t *action,
	       void *arg, dns_rdataset_t *nsrdataset, dns_name_t *zone,
	       dns_adbhandle_t **handle)
{
	REQUIRE(DNS_ADB_VALID(adb));
	if (task != NULL) {
		REQUIRE(action != NULL);
	}
	REQUIRE(nsrdataset != NULL);
	REQUIRE(zone != NULL);
	REQUIRE(handle != NULL && *handle == NULL);

	/*
	 * Iterate through the nsrdataset.  For each name found, do a search
	 * for it in our database.
	 *
	 * Possibilities:  Note that these are not always exclusive.
	 *
	 *	No name found.  In this case, allocate a new name header,
	 *	an initial namehook or two, and a job id.  If any of these
	 *	allocations fail, clean up and simply skip this address.
	 *
	 *	Name found, valid addresses present.  Allocate one addrinfo
	 *	structure for each found and append it to the linked list
	 *	of addresses for this header.
	 *
	 *	Name found, queries pending.  In this case, if a task was
	 *	passed in, allocate a job id, attach it to the name's job
	 *	list and remember to tell the caller that there will be
	 *	more info coming later.
	 */

	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_adb_refresh(dns_adb_t *adb, isc_task_t *task, isc_taskaction_t *action,
		void *arg, dns_rdataset_t *nsrdataset, dns_name_t *zone,
		dns_adbhandle_t *handle)
{
	REQUIRE(DNS_ADB_VALID(adb));
	if (task != NULL) {
		REQUIRE(action != NULL);
	}
	REQUIRE(nsrdataset != NULL);
	REQUIRE(zone != NULL);
	REQUIRE(DNS_ADBHANDLE_VALID(handle));

	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_adb_deletename(dns_adb_t *adb, dns_name_t *host)
{
	int name_bucket, addr_bucket;
	dns_adbname_t *name;
	dns_adbentry_t *entry;
	dns_adbnamehook_t *namehook;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(host != NULL);

	name = NULL;
	entry = NULL;
	namehook = NULL;

	/*
	 * Find the name.
	 */
	name_bucket = DNS_ADB_INVALIDBUCKET;
	name = find_name_and_lock(adb, host, &name_bucket);
	if (name == NULL) {
		UNLOCK(&adb->namelocks[name_bucket]);
		return (ISC_R_NOTFOUND);
	}

	/* XXX
	 * If any jobs are attached to this name, notify them that things
	 * are going away by canceling their requests.
	 */

	/*
	 * Loop through the name and kill any namehooks and entries they
	 * point to.
	 */
	addr_bucket = DNS_ADB_INVALIDBUCKET;
	namehook = ISC_LIST_HEAD(name->namehooks);
	while (namehook != NULL) {
		INSIST(DNS_ADBNAMEHOOK_VALID(namehook));

		/*
		 * Clean up the entry if needed.
		 */
		entry = namehook->entry;
		if (entry != NULL) {
			INSIST(DNS_ADBENTRY_VALID(entry));

			if (addr_bucket != entry->lock_bucket) {
				if (addr_bucket != DNS_ADB_INVALIDBUCKET)
					UNLOCK(&adb->entrylocks[addr_bucket]);
				addr_bucket = entry->lock_bucket;
				LOCK(&adb->entrylocks[addr_bucket]);
			}

			INSIST(entry->refcount > 0);
			entry->refcount--;
			if (entry->refcount == 0) {
				/* XXX kill zoneinfo here */
				ISC_LIST_UNLINK(adb->entries[addr_bucket],
						entry, link);
				entry->lock_bucket = DNS_ADB_INVALIDBUCKET;
				free_adbentry(adb, &entry);
			}
		}

		/*
		 * Free the namehook
		 */
		namehook->entry = NULL;
		ISC_LIST_UNLINK(name->namehooks, namehook, link);
		free_adbnamehook(adb, &namehook);

		namehook = ISC_LIST_HEAD(name->namehooks);
	}

	/*
	 * And lastly, unlink and free the name.
	 */
	ISC_LIST_UNLINK(adb->names[name_bucket], name, link);
	free_adbname(adb, &name);

	if (name_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->namelocks[name_bucket]);
	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);

	return (DNS_R_SUCCESS);
}

isc_result_t
dns_adb_insert(dns_adb_t *adb, dns_name_t *host, isc_sockaddr_t *addr)
{
	dns_adbname_t *name;
	isc_boolean_t free_name;
	dns_adbentry_t *entry;
	isc_boolean_t free_entry;
	dns_adbnamehook_t *namehook;
	isc_boolean_t free_namehook;
	int name_bucket, addr_bucket; /* unlock if != DNS_ADB_INVALIDBUCKET */
	isc_result_t result;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(host != NULL);
	REQUIRE(addr != NULL);

	name = NULL;
	free_name = ISC_FALSE;
	entry = NULL;
	free_entry = ISC_FALSE;
	namehook = NULL;
	free_namehook = ISC_FALSE;
	result = ISC_R_UNEXPECTED;

	/*
	 * First, see if the host is already in the database.  If it is,
	 * don't make a new host entry.  If not, copy the name and name's
	 * contents into our structure and allocate what we'll need
	 * to attach things together.
	 */
	name_bucket = DNS_ADB_INVALIDBUCKET;
	name = find_name_and_lock(adb, host, &name_bucket);
	if (name == NULL) {
		name = new_adbname(adb);
		if (name == NULL) {
			result = ISC_R_NOMEMORY;
			goto out;
		}
		free_name = ISC_TRUE;
		result = dns_name_dup(host, adb->mctx, &name->name);
		if (result != ISC_R_SUCCESS)
			goto out;
	}

	/*
	 * Now, while keeping the name locked, search for the address.
	 * Three possibilities:  One, the address doesn't exist.
	 * Two, the address exists, but we aren't linked to it.
	 * Three, the address exists and we are linked to it.
	 * (1) causes a new entry and namehook to be created.
	 * (2) causes only a new namehook.
	 * (3) is an error.
	 */
	addr_bucket = DNS_ADB_INVALIDBUCKET;
	entry = find_entry_and_lock(adb, addr, &addr_bucket);
	/*
	 * Case (1):  new entry and namehook.
	 */
	if (entry == NULL) {
		entry = new_adbentry(adb);
		if (entry == NULL) {
			result = ISC_R_NOMEMORY;
			goto out;
		}
		free_entry = ISC_TRUE;
	}

	/*
	 * Case (3):  entry exists, we're linked.
	 */
	namehook = ISC_LIST_HEAD(name->namehooks);
	while (namehook != NULL) {
		if (namehook->entry == entry) {
			result = ISC_R_EXISTS;
			goto out;
		}
	}

	/*
	 * Case (2):  New namehook, link to entry from above.
	 */
	namehook = new_adbnamehook(adb, entry);
	if (namehook == NULL) {
		result = ISC_R_NOMEMORY;
		goto out;
	}
	free_namehook = ISC_TRUE;
	ISC_LIST_APPEND(name->namehooks, namehook, link);

	entry->lock_bucket = addr_bucket;
	entry->refcount++;
	entry->sockaddr = *addr;

	/*
	 * If needed, string up the name and entry.  Do the name last, since
	 * adding multiple addresses is simplified in that case.
	 */
	if (!ISC_LINK_LINKED(name, link))
		ISC_LIST_PREPEND(adb->names[name_bucket], name, link);
	if (!ISC_LINK_LINKED(entry, link))
		ISC_LIST_PREPEND(adb->entries[addr_bucket], entry, link);
	UNLOCK(&adb->namelocks[name_bucket]);
	name_bucket = DNS_ADB_INVALIDBUCKET;
	UNLOCK(&adb->entrylocks[addr_bucket]);
	addr_bucket = DNS_ADB_INVALIDBUCKET;

	return (ISC_R_SUCCESS);

 out:
	if (free_name)
		free_adbname(adb, &name);
	if (free_entry)
		isc_mempool_put(adb->emp, entry);
	if (free_namehook)
		isc_mempool_put(adb->nhmp, namehook);
	if (name_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->namelocks[name_bucket]);
	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);

	return (result);
}

void
dns_adb_cancel(dns_adb_t *adb, dns_adbhandle_t **handle)
{
	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(handle != NULL && DNS_ADBHANDLE_VALID(*handle));

	INSIST(1 == 0);
}

void
dns_adb_done(dns_adb_t *adb, dns_adbhandle_t **handle)
{
	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(handle != NULL && DNS_ADBHANDLE_VALID(*handle));

	INSIST(1 == 0);
}

void
dns_adb_dump(dns_adb_t *adb, FILE *f)
{
	int i;
	isc_sockaddr_t *sa;
	dns_adbname_t *name;
	dns_adbentry_t *entry;
	char tmp[512];
	char *tmpp;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(f != NULL);

	/*
	 * Lock the adb itself, lock all the name buckets, then lock all
	 * the entry buckets.  This should put the adb into a state where
	 * nothing can change, so we can iterate through everything and
	 * print at our leasure.
	 */

	LOCK(&adb->lock);

	for (i = 0 ; i < DNS_ADBNAMELIST_LENGTH ; i++)
		LOCK(&adb->namelocks[i]);
	for (i = 0 ; i < DNS_ADBENTRYLIST_LENGTH ; i++)
		LOCK(&adb->entrylocks[i]);

	/*
	 * Dump the names
	 */
	fprintf(f, "Names:\n");
	for (i = 0 ; i < DNS_ADBNAMELIST_LENGTH ; i++) {
		name = ISC_LIST_HEAD(adb->names[i]);
		if (name == NULL)
			continue;
		fprintf(f, "Name bucket %d:\n", i);
		while (name != NULL) {
			fprintf(f, "name %p\n", name);
			if (!DNS_ADBNAME_VALID(name))
				fprintf(f, "\tMAGIC %08x\n", name->magic);
			fprintf(f, "\t");
			print_dns_name(f, &name->name);
			print_namehook_list(f, name);

			name = ISC_LIST_NEXT(name, link);
		}
	}

	/*
	 * Dump the entries
	 */
	fprintf(f, "Entries:\n");
	for (i = 0 ; i < DNS_ADBENTRYLIST_LENGTH ; i++) {
		entry = ISC_LIST_HEAD(adb->entries[i]);
		if (entry == NULL)
			continue;
		fprintf(f, "Entry bucket %d:\n", i);
		while (entry != NULL) {
			if (!DNS_ADBENTRY_VALID(entry))
				fprintf(f, "\tMAGIC %08x\n", entry->magic);
			if (entry->lock_bucket != i)
				fprintf(f, "\tWRONG BUCKET!  lock_bucket %d\n",
					entry->lock_bucket);

			sa = &entry->sockaddr;
			tmpp = inet_ntop(sa->type.sa.sa_family, &sa->type.sa,
					 tmp, sizeof tmp);
			if (tmpp == NULL)
				tmpp = "CANNOT TRANSLATE ADDRESS!";

			fprintf(f, "\trefcount %u flags %08x goodness %d"
				" srtt %u host %s\n",
				entry->refcount, entry->flags, entry->goodness,
				entry->srtt, tmpp);

			entry = ISC_LIST_NEXT(entry, link);
		}
	}

 out:
	/*
	 * Unlock everything
	 */
	for (i = 0 ; i < DNS_ADBENTRYLIST_LENGTH ; i++)
		UNLOCK(&adb->entrylocks[i]);
	for (i = 0 ; i < DNS_ADBNAMELIST_LENGTH ; i++)
		UNLOCK(&adb->namelocks[i]);
	UNLOCK(&adb->lock);
}

static void
print_dns_name(FILE *f, dns_name_t *name)
{
	char buf[257];
	isc_buffer_t b;

	INSIST(f != NULL);

	memset(buf, 0, sizeof (buf));
	isc_buffer_init(&b, buf, sizeof (buf) - 1, ISC_BUFFERTYPE_TEXT);

	dns_name_totext(name, ISC_FALSE, &b);
	fprintf(f, buf); /* safe, since names < 256 chars, and we memset */
}

static void
print_namehook_list(FILE *f, dns_adbname_t *n)
{
	dns_adbnamehook_t *nh;

	nh = ISC_LIST_HEAD(n->namehooks);
	if (nh == NULL)
		fprintf(f, "\t\tNo name hooks\n");

	while (nh != NULL) {
		fprintf(f, "\t\tHook %p -> entry %p\n", nh, nh->entry);
		nh = ISC_LIST_NEXT(nh, link);
	}
}
