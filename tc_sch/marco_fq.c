// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/sched/sch_fq.c Fair Queue Packet Scheduler (per flow pacing)
 *
 *  Copyright (C) 2013-2015 Eric Dumazet <edumazet@google.com>
 *
 *  Meant to be mostly used for locally generated traffic :
 *  Fast classification depends on skb->sk being set before reaching us.
 *  If not, (router workload), we use rxhash as fallback, with 32 bits wide hash.
 *  All packets belonging to a socket are considered as a 'flow'.
 *
 *  Flows are dynamically allocated and stored in a hash table of RB trees
 *  They are also part of one Round Robin 'queues' (new or old flows)
 *
 *  Burst avoidance (aka pacing) capability :
 *
 *  Transport (eg TCP) can set in sk->sk_pacing_rate a rate, enqueue a
 *  bunch of packets, and this packet scheduler adds delay between
 *  packets to respect rate limitation.
 *
 *  enqueue() :
 *   - lookup one RB tree (out of 1024 or more) to find the flow.
 *     If non existent flow, create it, add it to the tree.
 *     Add skb to the per flow list of skb (fifo).
 *   - Use a special fifo for high prio packets
 *
 *  dequeue() : serves flows in Round Robin
 *  Note : When a flow becomes empty, we do not immediately remove it from
 *  rb trees, for performance reasons (its expected to send additional packets,
 *  or SLAB cache will reuse socket for another flow)
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/hash.h>
#include <linux/prefetch.h>
#include <linux/vmalloc.h>
#include <linux/hashtable.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>

DEFINE_HASHTABLE(ip_count_table, 16); // 16 is the number of bits in the hash table

struct hash_ip_count
{
    __be32 s_ip;
    __be32 d_ip;             // Source IP address
    int count;               // Number of packets from this source IP
    struct hlist_node hnode; // Node for hash table
};

struct marco_fq_skb_cb
{
    u64 time_to_send;
};

static inline struct marco_fq_skb_cb *marco_fq_skb_cb(struct sk_buff *skb)
{
    qdisc_cb_private_validate(skb, sizeof(struct marco_fq_skb_cb));
    return (struct marco_fq_skb_cb *)qdisc_skb_cb(skb)->data;
}

/*
 * Per flow structure, dynamically allocated.
 * If packets have monotically increasing time_to_send, they are placed in O(1)
 * in linear list (head,tail), otherwise are placed in a rbtree (t_root).
 */
struct marco_fq_flow
{
    /* First cache line : used in marco_fq_gc(), marco_fq_enqueue(), marco_fq_dequeue() */
    struct rb_root t_root;
    struct sk_buff *head; /* list of skbs for this flow : first skb */
    union
    {
        struct sk_buff *tail; /* last skb in the list */
        unsigned long age;    /* (jiffies | 1UL) when flow was emptied, for gc */
    };
    struct rb_node fq_node; /* anchor in fq_root[] trees */
    struct sock *sk;
    u32 socket_hash; /* sk_hash */
    int qlen;        /* number of packets in flow queue */

    /* Second cache line, used in marco_fq_dequeue() */
    int credit;
    /* 32bit hole on 64bit arches */

    struct marco_fq_flow *next; /* next pointer in RR lists */

    struct rb_node rate_node; /* anchor in q->delayed tree */
    u64 time_next_packet;
} ____cacheline_aligned_in_smp;

struct marco_fq_flow_head
{
    struct marco_fq_flow *first;
    struct marco_fq_flow *last;
};

struct marco_fq_sched_data
{
    struct marco_fq_flow_head new_flows;

    struct marco_fq_flow_head old_flows;

    struct rb_root delayed; /* for rate limited flows */
    u64 time_next_delayed_flow;
    u64 ktime_cache; /* copy of last ktime_get_ns() */
    unsigned long unthrottle_latency_ns;

    struct marco_fq_flow internal; /* for non classified or high prio packets */
    u32 quantum;
    u32 initial_quantum;
    u32 flow_refill_delay;
    u32 flow_plimit;             /* max packets per flow */
    unsigned long flow_max_rate; /* optional max rate per flow */
    u64 ce_threshold;
    u64 horizon;     /* horizon in ns */
    u32 orphan_mask; /* mask for orphaned skb */
    u32 low_rate_threshold;
    struct rb_root *fq_root;
    u8 rate_enable;
    u8 fq_trees_log;
    u8 horizon_drop;
    u32 flows;
    u32 inactive_flows;
    u32 throttled_flows;

    u64 stat_gc_flows;
    u64 stat_internal_packets;
    u64 stat_throttled;
    u64 stat_ce_mark;
    u64 stat_horizon_drops;
    u64 stat_horizon_caps;
    u64 stat_flows_plimit;
    u64 stat_pkts_too_long;
    u64 stat_allocation_errors;

    u32 timer_slack; /* hrtimer slack in ns */
    struct qdisc_watchdog watchdog;
};

/*
 * f->tail and f->age share the same location.
 * We can use the low order bit to differentiate if this location points
 * to a sk_buff or contains a jiffies value, if we force this value to be odd.
 * This assumes f->tail low order bit must be 0 since alignof(struct sk_buff) >= 2
 */
static void marco_fq_flow_set_detached(struct marco_fq_flow *f)
{
    f->age = jiffies | 1UL;
}

static bool marco_fq_flow_is_detached(const struct marco_fq_flow *f)
{
    return !!(f->age & 1UL);
}

/* special value to mark a throttled flow (not on old/new list) */
static struct marco_fq_flow throttled;

static bool marco_fq_flow_is_throttled(const struct marco_fq_flow *f)
{
    return f->next == &throttled;
}

static void marco_fq_flow_add_tail(struct marco_fq_flow_head *head, struct marco_fq_flow *flow)
{
    if (head->first)
        head->last->next = flow;
    else
        head->first = flow;
    head->last = flow;
    flow->next = NULL;
}

static void marco_fq_flow_unset_throttled(struct marco_fq_sched_data *q, struct marco_fq_flow *f)
{
    rb_erase(&f->rate_node, &q->delayed);
    q->throttled_flows--;
    marco_fq_flow_add_tail(&q->old_flows, f);
}

static void marco_fq_flow_set_throttled(struct marco_fq_sched_data *q, struct marco_fq_flow *f)
{
    struct rb_node **p = &q->delayed.rb_node, *parent = NULL;

    while (*p)
    {
        struct marco_fq_flow *aux;

        parent = *p;
        aux = rb_entry(parent, struct marco_fq_flow, rate_node);
        if (f->time_next_packet >= aux->time_next_packet)
            p = &parent->rb_right;
        else
            p = &parent->rb_left;
    }
    rb_link_node(&f->rate_node, parent, p);
    rb_insert_color(&f->rate_node, &q->delayed);
    q->throttled_flows++;
    q->stat_throttled++;

    f->next = &throttled;
    if (q->time_next_delayed_flow > f->time_next_packet)
        q->time_next_delayed_flow = f->time_next_packet;
}

static struct kmem_cache *marco_fq_flow_cachep __read_mostly;

/* limit number of collected flows per round */
#define FQ_GC_MAX 8
#define FQ_GC_AGE (3 * HZ)

static bool marco_fq_gc_candidate(const struct marco_fq_flow *f)
{
    return marco_fq_flow_is_detached(f) &&
           time_after(jiffies, f->age + FQ_GC_AGE);
}

static void marco_fq_gc(struct marco_fq_sched_data *q,
                  struct rb_root *root,
                  struct sock *sk)
{
    struct rb_node **p, *parent;
    void *tofree[FQ_GC_MAX];
    struct marco_fq_flow *f;
    int i, fcnt = 0;

    p = &root->rb_node;
    parent = NULL;
    while (*p)
    {
        parent = *p;

        f = rb_entry(parent, struct marco_fq_flow, fq_node);
        if (f->sk == sk)
            break;

        if (marco_fq_gc_candidate(f))
        {
            tofree[fcnt++] = f;
            if (fcnt == FQ_GC_MAX)
                break;
        }

        if (f->sk > sk)
            p = &parent->rb_right;
        else
            p = &parent->rb_left;
    }

    if (!fcnt)
        return;

    for (i = fcnt; i > 0;)
    {
        f = tofree[--i];
        rb_erase(&f->fq_node, root);
    }
    q->flows -= fcnt;
    q->inactive_flows -= fcnt;
    q->stat_gc_flows += fcnt;

    kmem_cache_free_bulk(marco_fq_flow_cachep, fcnt, tofree);
}

static struct marco_fq_flow *marco_fq_classify(struct sk_buff *skb, struct marco_fq_sched_data *q)
{
    struct rb_node **p, *parent;
    struct sock *sk = skb->sk;
    struct rb_root *root;
    struct marco_fq_flow *f;

    /* warning: no starvation prevention... */
    if (unlikely((skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL))
        return &q->internal;

    /* SYNACK messages are attached to a TCP_NEW_SYN_RECV request socket
     * or a listener (SYNCOOKIE mode)
     * 1) request sockets are not full blown,
     *    they do not contain sk_pacing_rate
     * 2) They are not part of a 'flow' yet
     * 3) We do not want to rate limit them (eg SYNFLOOD attack),
     *    especially if the listener set SO_MAX_PACING_RATE
     * 4) We pretend they are orphaned
     */
    if (!sk || sk_listener(sk))
    {
        unsigned long hash = skb_get_hash(skb) & q->orphan_mask;

        /* By forcing low order bit to 1, we make sure to not
         * collide with a local flow (socket pointers are word aligned)
         */
        sk = (struct sock *)((hash << 1) | 1UL);
        skb_orphan(skb);
    }
    else if (sk->sk_state == TCP_CLOSE)
    {
        unsigned long hash = skb_get_hash(skb) & q->orphan_mask;
        /*
         * Sockets in TCP_CLOSE are non connected.
         * Typical use case is UDP sockets, they can send packets
         * with sendto() to many different destinations.
         * We probably could use a generic bit advertising
         * non connected sockets, instead of sk_state == TCP_CLOSE,
         * if we care enough.
         */
        sk = (struct sock *)((hash << 1) | 1UL);
    }

    root = &q->fq_root[hash_ptr(sk, q->fq_trees_log)];

    if (q->flows >= (2U << q->fq_trees_log) &&
        q->inactive_flows > q->flows / 2)
        marco_fq_gc(q, root, sk);

    p = &root->rb_node;
    parent = NULL;
    while (*p)
    {
        parent = *p;

        f = rb_entry(parent, struct marco_fq_flow, fq_node);
        if (f->sk == sk)
        {
            /* socket might have been reallocated, so check
             * if its sk_hash is the same.
             * It not, we need to refill credit with
             * initial quantum
             */
            if (unlikely(skb->sk == sk &&
                         f->socket_hash != sk->sk_hash))
            {
                f->credit = q->initial_quantum;
                f->socket_hash = sk->sk_hash;
                if (q->rate_enable)
                    smp_store_release(&sk->sk_pacing_status,
                                      SK_PACING_FQ);
                if (marco_fq_flow_is_throttled(f))
                    marco_fq_flow_unset_throttled(q, f);
                f->time_next_packet = 0ULL;
            }
            return f;
        }
        if (f->sk > sk)
            p = &parent->rb_right;
        else
            p = &parent->rb_left;
    }

    f = kmem_cache_zalloc(marco_fq_flow_cachep, GFP_ATOMIC | __GFP_NOWARN);
    if (unlikely(!f))
    {
        q->stat_allocation_errors++;
        return &q->internal;
    }
    /* f->t_root is already zeroed after kmem_cache_zalloc() */

    marco_fq_flow_set_detached(f);
    f->sk = sk;
    if (skb->sk == sk)
    {
        f->socket_hash = sk->sk_hash;
        if (q->rate_enable)
            smp_store_release(&sk->sk_pacing_status,
                              SK_PACING_FQ);
    }
    f->credit = q->initial_quantum;

    rb_link_node(&f->fq_node, parent, p);
    rb_insert_color(&f->fq_node, root);

    q->flows++;
    q->inactive_flows++;
    return f;
}

static struct sk_buff *marco_fq_peek(struct marco_fq_flow *flow)
{
    struct sk_buff *skb = skb_rb_first(&flow->t_root);
    struct sk_buff *head = flow->head;

    if (!skb)
        return head;

    if (!head)
        return skb;

    if (marco_fq_skb_cb(skb)->time_to_send < marco_fq_skb_cb(head)->time_to_send)
        return skb;
    return head;
}

static void marco_fq_erase_head(struct Qdisc *sch, struct marco_fq_flow *flow,
                          struct sk_buff *skb)
{
    if (skb == flow->head)
    {
        flow->head = skb->next;
    }
    else
    {
        rb_erase(&skb->rbnode, &flow->t_root);
        skb->dev = qdisc_dev(sch);
    }
}

/* Remove one skb from flow queue.
 * This skb must be the return value of prior marco_fq_peek().
 */
static void marco_fq_dequeue_skb(struct Qdisc *sch, struct marco_fq_flow *flow,
                           struct sk_buff *skb)
{
    marco_fq_erase_head(sch, flow, skb);
    skb_mark_not_on_list(skb);
    flow->qlen--;
    qdisc_qstats_backlog_dec(sch, skb);
    sch->q.qlen--;
}

static void marco_flow_queue_add(struct marco_fq_flow *flow, struct sk_buff *skb)
{
    struct rb_node **p, *parent;
    struct sk_buff *head, *aux;

    head = flow->head;
    if (!head ||
        marco_fq_skb_cb(skb)->time_to_send >= marco_fq_skb_cb(flow->tail)->time_to_send)
    {
        if (!head)
            flow->head = skb;
        else
            flow->tail->next = skb;
        flow->tail = skb;
        skb->next = NULL;
        return;
    }

    p = &flow->t_root.rb_node;
    parent = NULL;

    while (*p)
    {
        parent = *p;
        aux = rb_to_skb(parent);
        if (marco_fq_skb_cb(skb)->time_to_send >= marco_fq_skb_cb(aux)->time_to_send)
            p = &parent->rb_right;
        else
            p = &parent->rb_left;
    }
    rb_link_node(&skb->rbnode, parent, p);
    rb_insert_color(&skb->rbnode, &flow->t_root);
}

static bool marco_fq_packet_beyond_horizon(const struct sk_buff *skb,
                                     const struct marco_fq_sched_data *q)
{
    return unlikely((s64)skb->tstamp > (s64)(q->ktime_cache + q->horizon));
}

static int marco_fq_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                      struct sk_buff **to_free)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct marco_fq_flow *f;

    if (unlikely(sch->q.qlen >= sch->limit)){
        printk("The queue is full\n");
        return qdisc_drop(skb, sch, to_free);
    }

    if (!skb->tstamp)
    {
        marco_fq_skb_cb(skb)->time_to_send = q->ktime_cache = ktime_get_ns();
    }
    else
    {
        /* Check if packet timestamp is too far in the future.
         * Try first if our cached value, to avoid ktime_get_ns()
         * cost in most cases.
         */
        if (marco_fq_packet_beyond_horizon(skb, q))
        {
            /* Refresh our cache and check another time */
            q->ktime_cache = ktime_get_ns();
            if (marco_fq_packet_beyond_horizon(skb, q))
            {
                if (q->horizon_drop)
                {
                    q->stat_horizon_drops++;
                    return qdisc_drop(skb, sch, to_free);
                }
                q->stat_horizon_caps++;
                skb->tstamp = q->ktime_cache + q->horizon;
            }
        }
        marco_fq_skb_cb(skb)->time_to_send = skb->tstamp;
    }

    f = marco_fq_classify(skb, q);
    if (unlikely(f->qlen >= q->flow_plimit && f != &q->internal))
    {
        q->stat_flows_plimit++;
        return qdisc_drop(skb, sch, to_free);
    }

    f->qlen++;
    qdisc_qstats_backlog_inc(sch, skb);
    if (marco_fq_flow_is_detached(f))
    {
        marco_fq_flow_add_tail(&q->new_flows, f);
        if (time_after(jiffies, f->age + q->flow_refill_delay))
            f->credit = max_t(u32, f->credit, q->quantum);
        q->inactive_flows--;
    }

    /* Note: this overwrites f->age */
    printk("add to flow queue\n");
    struct iphdr *iph = ip_hdr(skb);
    __be32 des_ip = iph->daddr;
    __be32 src_ip = iph->saddr;

    struct hash_ip_count *ip_count;
    unsigned int key = jhash_1word((__force u32)des_ip, 0);

    hash_for_each_possible(ip_count_table, ip_count, hnode, key)
    {
        if (ip_count->d_ip == des_ip && ip_count->s_ip == src_ip)
        {
            ip_count->count++;
            char buf[16];
            sprintf(buf, "%pI4", &des_ip);
            printk("income des ip_count->count: %d\t%s\n", ip_count->count, buf);
            break;
        }
    }

    if (!ip_count)
    {
        ip_count = kmalloc(sizeof(*ip_count), GFP_ATOMIC);
        if (!ip_count)
            return ENOMEM;

        ip_count->d_ip = des_ip;
        ip_count->s_ip = src_ip;
        ip_count->count = 1;
        hash_add(ip_count_table, &ip_count->hnode, key);
        printk("New ip");
        char buf[16];
        sprintf(buf, "%pI4", &des_ip);
        printk("Des IP: %s\n", buf);
    }
    marco_flow_queue_add(f, skb);

    if (unlikely(f == &q->internal))
    {
        q->stat_internal_packets++;
    }
    sch->q.qlen++;

    return NET_XMIT_SUCCESS;
}

static void marco_fq_check_throttled(struct marco_fq_sched_data *q, u64 now)
{
    unsigned long sample;
    struct rb_node *p;

    if (q->time_next_delayed_flow > now)
        return;

    /* Update unthrottle latency EWMA.
     * This is cheap and can help diagnosing timer/latency problems.
     */
    sample = (unsigned long)(now - q->time_next_delayed_flow);
    q->unthrottle_latency_ns -= q->unthrottle_latency_ns >> 3;
    q->unthrottle_latency_ns += sample >> 3;

    q->time_next_delayed_flow = ~0ULL;
    while ((p = rb_first(&q->delayed)) != NULL)
    {
        struct marco_fq_flow *f = rb_entry(p, struct marco_fq_flow, rate_node);

        if (f->time_next_packet > now)
        {
            q->time_next_delayed_flow = f->time_next_packet;
            break;
        }
        marco_fq_flow_unset_throttled(q, f);
    }
}

static struct sk_buff *marco_fq_dequeue(struct Qdisc *sch)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct marco_fq_flow_head *head;
    struct sk_buff *skb;
    struct marco_fq_flow *f;
    unsigned long rate;
    u32 plen;
    u64 now;

    if (!sch->q.qlen)
        return NULL;

    skb = marco_fq_peek(&q->internal);
    if (unlikely(skb))
    {
        marco_fq_dequeue_skb(sch, &q->internal, skb);
        goto out;
    }

    q->ktime_cache = now = ktime_get_ns();
    marco_fq_check_throttled(q, now);
begin:
    head = &q->new_flows;
    if (!head->first)
    {
        head = &q->old_flows;
        if (!head->first)
        {
            if (q->time_next_delayed_flow != ~0ULL)
                qdisc_watchdog_schedule_range_ns(&q->watchdog,
                                                 q->time_next_delayed_flow,
                                                 q->timer_slack);
            return NULL;
        }
    }
    f = head->first;

    if (f->credit <= 0)
    {
        f->credit += q->quantum;
        head->first = f->next;
        marco_fq_flow_add_tail(&q->old_flows, f);
        goto begin;
    }

    skb = marco_fq_peek(f);
    if (skb)
    {
        u64 time_next_packet = max_t(u64, marco_fq_skb_cb(skb)->time_to_send,
                                     f->time_next_packet);

        // get the source ip
        struct iphdr *iph = ip_hdr(skb);
        __be32 des_ip = iph->daddr;
        __be32 src_ip = iph->saddr;

        struct hash_ip_count *ip_count;
        unsigned int key = jhash_1word((__force u32)src_ip, 0); // check for source ip as it should be the output flow
        hash_for_each_possible(ip_count_table, ip_count, hnode, key)
        {
            if (ip_count->s_ip == des_ip && ip_count->d_ip == src_ip && ip_count->count > 0)
            {
                ip_count->count--;
                char buf[16];

                // add delay if the output package is > 10
                if (ip_count->count > 5)
                {
                    sprintf(buf, "%pI4", &des_ip);
                    printk("ip_count->count: %d\t source:%s\n", ip_count->count, buf);
                    time_next_packet += 10000000;
                    printk("added 10 ms");
                }
            }
        }

        if (now < time_next_packet)
        {
            head->first = f->next;
            f->time_next_packet = time_next_packet;
            marco_fq_flow_set_throttled(q, f);
            goto begin;
        }
        prefetch(&skb->end);
        if ((s64)(now - time_next_packet - q->ce_threshold) > 0)
        {
            INET_ECN_set_ce(skb);
            q->stat_ce_mark++;
        }
        marco_fq_dequeue_skb(sch, f, skb);
    }
    else
    {
        head->first = f->next;
        /* force a pass through old_flows to prevent starvation */
        if ((head == &q->new_flows) && q->old_flows.first)
        {
            marco_fq_flow_add_tail(&q->old_flows, f);
        }
        else
        {
            marco_fq_flow_set_detached(f);
            q->inactive_flows++;
        }
        goto begin;
    }
    plen = qdisc_pkt_len(skb);
    f->credit -= plen;

    if (!q->rate_enable)
        goto out;

    rate = q->flow_max_rate;

    /* If EDT time was provided for this skb, we need to
     * update f->time_next_packet only if this qdisc enforces
     * a flow max rate.
     */
    if (!skb->tstamp)
    {
        if (skb->sk)
            rate = min(skb->sk->sk_pacing_rate, rate);

        if (rate <= q->low_rate_threshold)
        {
            f->credit = 0;
        }
        else
        {
            plen = max(plen, q->quantum);
            if (f->credit > 0)
                goto out;
        }
    }
    if (rate != ~0UL)
    {
        u64 len = (u64)plen * NSEC_PER_SEC;

        if (likely(rate))
            len = div64_ul(len, rate);
        /* Since socket rate can change later,
         * clamp the delay to 1 second.
         * Really, providers of too big packets should be fixed !
         */
        if (unlikely(len > NSEC_PER_SEC))
        {
            len = NSEC_PER_SEC;
            q->stat_pkts_too_long++;
        }
        /* Account for schedule/timers drifts.
         * f->time_next_packet was set when prior packet was sent,
         * and current time (@now) can be too late by tens of us.
         */
        if (f->time_next_packet)
            len -= min(len / 2, now - f->time_next_packet);
        f->time_next_packet = now + len;
    }
out:
    qdisc_bstats_update(sch, skb);
    return skb;
}

static void marco_fq_flow_purge(struct marco_fq_flow *flow)
{
    struct rb_node *p = rb_first(&flow->t_root);

    while (p)
    {
        struct sk_buff *skb = rb_to_skb(p);

        p = rb_next(p);
        rb_erase(&skb->rbnode, &flow->t_root);
        rtnl_kfree_skbs(skb, skb);
    }
    rtnl_kfree_skbs(flow->head, flow->tail);
    flow->head = NULL;
    flow->qlen = 0;
}

static void marco_fq_reset(struct Qdisc *sch)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct rb_root *root;
    struct rb_node *p;
    struct marco_fq_flow *f;
    unsigned int idx;

    sch->q.qlen = 0;
    sch->qstats.backlog = 0;

    marco_fq_flow_purge(&q->internal);

    if (!q->fq_root)
        return;

    for (idx = 0; idx < (1U << q->fq_trees_log); idx++)
    {
        root = &q->fq_root[idx];
        while ((p = rb_first(root)) != NULL)
        {
            f = rb_entry(p, struct marco_fq_flow, fq_node);
            rb_erase(p, root);

            marco_fq_flow_purge(f);

            kmem_cache_free(marco_fq_flow_cachep, f);
        }
    }
    q->new_flows.first = NULL;
    q->old_flows.first = NULL;
    q->delayed = RB_ROOT;
    q->flows = 0;
    q->inactive_flows = 0;
    q->throttled_flows = 0;
}

static void marco_fq_rehash(struct marco_fq_sched_data *q,
                      struct rb_root *old_array, u32 old_log,
                      struct rb_root *new_array, u32 new_log)
{
    struct rb_node *op, **np, *parent;
    struct rb_root *oroot, *nroot;
    struct marco_fq_flow *of, *nf;
    int fcnt = 0;
    u32 idx;

    for (idx = 0; idx < (1U << old_log); idx++)
    {
        oroot = &old_array[idx];
        while ((op = rb_first(oroot)) != NULL)
        {
            rb_erase(op, oroot);
            of = rb_entry(op, struct marco_fq_flow, fq_node);
            if (marco_fq_gc_candidate(of))
            {
                fcnt++;
                kmem_cache_free(marco_fq_flow_cachep, of);
                continue;
            }
            nroot = &new_array[hash_ptr(of->sk, new_log)];

            np = &nroot->rb_node;
            parent = NULL;
            while (*np)
            {
                parent = *np;

                nf = rb_entry(parent, struct marco_fq_flow, fq_node);
                BUG_ON(nf->sk == of->sk);

                if (nf->sk > of->sk)
                    np = &parent->rb_right;
                else
                    np = &parent->rb_left;
            }

            rb_link_node(&of->fq_node, parent, np);
            rb_insert_color(&of->fq_node, nroot);
        }
    }
    q->flows -= fcnt;
    q->inactive_flows -= fcnt;
    q->stat_gc_flows += fcnt;
}

static void marco_fq_free(void *addr)
{
    kvfree(addr);
}

static int marco_fq_resize(struct Qdisc *sch, u32 log)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct rb_root *array;
    void *old_fq_root;
    u32 idx;

    if (q->fq_root && log == q->fq_trees_log)
        return 0;

    /* If XPS was setup, we can allocate memory on right NUMA node */
    array = kvmalloc_node(sizeof(struct rb_root) << log, GFP_KERNEL | __GFP_RETRY_MAYFAIL,
                          netdev_queue_numa_node_read(sch->dev_queue));
    if (!array)
        return -ENOMEM;

    for (idx = 0; idx < (1U << log); idx++)
        array[idx] = RB_ROOT;

    sch_tree_lock(sch);

    old_fq_root = q->fq_root;
    if (old_fq_root)
        marco_fq_rehash(q, old_fq_root, q->fq_trees_log, array, log);

    q->fq_root = array;
    q->fq_trees_log = log;

    sch_tree_unlock(sch);

    marco_fq_free(old_fq_root);

    return 0;
}

static const struct nla_policy fq_policy[TCA_FQ_MAX + 1] = {
    [TCA_FQ_UNSPEC] = {.strict_start_type = TCA_FQ_TIMER_SLACK},

    [TCA_FQ_PLIMIT] = {.type = NLA_U32},
    [TCA_FQ_FLOW_PLIMIT] = {.type = NLA_U32},
    [TCA_FQ_QUANTUM] = {.type = NLA_U32},
    [TCA_FQ_INITIAL_QUANTUM] = {.type = NLA_U32},
    [TCA_FQ_RATE_ENABLE] = {.type = NLA_U32},
    [TCA_FQ_FLOW_DEFAULT_RATE] = {.type = NLA_U32},
    [TCA_FQ_FLOW_MAX_RATE] = {.type = NLA_U32},
    [TCA_FQ_BUCKETS_LOG] = {.type = NLA_U32},
    [TCA_FQ_FLOW_REFILL_DELAY] = {.type = NLA_U32},
    [TCA_FQ_ORPHAN_MASK] = {.type = NLA_U32},
    [TCA_FQ_LOW_RATE_THRESHOLD] = {.type = NLA_U32},
    [TCA_FQ_CE_THRESHOLD] = {.type = NLA_U32},
    [TCA_FQ_TIMER_SLACK] = {.type = NLA_U32},
    [TCA_FQ_HORIZON] = {.type = NLA_U32},
    [TCA_FQ_HORIZON_DROP] = {.type = NLA_U8},
};

static int marco_fq_change(struct Qdisc *sch, struct nlattr *opt,
                     struct netlink_ext_ack *extack)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct nlattr *tb[TCA_FQ_MAX + 1];
    int err, drop_count = 0;
    unsigned drop_len = 0;
    u32 fq_log;

    if (!opt)
        return -EINVAL;

    err = nla_parse_nested_deprecated(tb, TCA_FQ_MAX, opt, fq_policy,
                                      NULL);
    if (err < 0)
        return err;

    sch_tree_lock(sch);

    fq_log = q->fq_trees_log;

    if (tb[TCA_FQ_BUCKETS_LOG])
    {
        u32 nval = nla_get_u32(tb[TCA_FQ_BUCKETS_LOG]);

        if (nval >= 1 && nval <= ilog2(256 * 1024))
            fq_log = nval;
        else
            err = -EINVAL;
    }
    if (tb[TCA_FQ_PLIMIT])
        sch->limit = nla_get_u32(tb[TCA_FQ_PLIMIT]);

    if (tb[TCA_FQ_FLOW_PLIMIT])
        q->flow_plimit = nla_get_u32(tb[TCA_FQ_FLOW_PLIMIT]);

    if (tb[TCA_FQ_QUANTUM])
    {
        u32 quantum = nla_get_u32(tb[TCA_FQ_QUANTUM]);

        if (quantum > 0 && quantum <= (1 << 20))
        {
            q->quantum = quantum;
        }
        else
        {
            NL_SET_ERR_MSG_MOD(extack, "invalid quantum");
            err = -EINVAL;
        }
    }

    if (tb[TCA_FQ_INITIAL_QUANTUM])
        q->initial_quantum = nla_get_u32(tb[TCA_FQ_INITIAL_QUANTUM]);

    if (tb[TCA_FQ_FLOW_DEFAULT_RATE])
        pr_warn_ratelimited("sch_fq: defrate %u ignored.\n",
                            nla_get_u32(tb[TCA_FQ_FLOW_DEFAULT_RATE]));

    if (tb[TCA_FQ_FLOW_MAX_RATE])
    {
        u32 rate = nla_get_u32(tb[TCA_FQ_FLOW_MAX_RATE]);

        q->flow_max_rate = (rate == ~0U) ? ~0UL : rate;
    }
    if (tb[TCA_FQ_LOW_RATE_THRESHOLD])
        q->low_rate_threshold =
            nla_get_u32(tb[TCA_FQ_LOW_RATE_THRESHOLD]);

    if (tb[TCA_FQ_RATE_ENABLE])
    {
        u32 enable = nla_get_u32(tb[TCA_FQ_RATE_ENABLE]);

        if (enable <= 1)
            q->rate_enable = enable;
        else
            err = -EINVAL;
    }

    if (tb[TCA_FQ_FLOW_REFILL_DELAY])
    {
        u32 usecs_delay = nla_get_u32(tb[TCA_FQ_FLOW_REFILL_DELAY]);

        q->flow_refill_delay = usecs_to_jiffies(usecs_delay);
    }

    if (tb[TCA_FQ_ORPHAN_MASK])
        q->orphan_mask = nla_get_u32(tb[TCA_FQ_ORPHAN_MASK]);

    if (tb[TCA_FQ_CE_THRESHOLD])
        q->ce_threshold = (u64)NSEC_PER_USEC *
                          nla_get_u32(tb[TCA_FQ_CE_THRESHOLD]);

    if (tb[TCA_FQ_TIMER_SLACK])
        q->timer_slack = nla_get_u32(tb[TCA_FQ_TIMER_SLACK]);

    if (tb[TCA_FQ_HORIZON])
        q->horizon = (u64)NSEC_PER_USEC *
                     nla_get_u32(tb[TCA_FQ_HORIZON]);

    if (tb[TCA_FQ_HORIZON_DROP])
        q->horizon_drop = nla_get_u8(tb[TCA_FQ_HORIZON_DROP]);

    if (!err)
    {

        sch_tree_unlock(sch);
        err = marco_fq_resize(sch, fq_log);
        sch_tree_lock(sch);
    }
    while (sch->q.qlen > sch->limit)
    {
        struct sk_buff *skb = marco_fq_dequeue(sch);

        if (!skb)
            break;
        drop_len += qdisc_pkt_len(skb);
        rtnl_kfree_skbs(skb, skb);
        drop_count++;
    }
    qdisc_tree_reduce_backlog(sch, drop_count, drop_len);

    sch_tree_unlock(sch);
    return err;
}

static void marco_fq_destroy(struct Qdisc *sch)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);

    marco_fq_reset(sch);
    marco_fq_free(q->fq_root);
    qdisc_watchdog_cancel(&q->watchdog);
}

static int marco_fq_init(struct Qdisc *sch, struct nlattr *opt,
                   struct netlink_ext_ack *extack)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    int err;

    sch->limit = 10000;
    q->flow_plimit = 100;
    q->quantum = 2 * psched_mtu(qdisc_dev(sch));
    q->initial_quantum = 10 * psched_mtu(qdisc_dev(sch));
    q->flow_refill_delay = msecs_to_jiffies(40);
    q->flow_max_rate = ~0UL;
    q->time_next_delayed_flow = ~0ULL;
    q->rate_enable = 1;
    q->new_flows.first = NULL;
    q->old_flows.first = NULL;
    q->delayed = RB_ROOT;
    q->fq_root = NULL;
    q->fq_trees_log = ilog2(1024);
    q->orphan_mask = 1024 - 1;
    q->low_rate_threshold = 550000 / 8;

    q->timer_slack = 10 * NSEC_PER_USEC; /* 10 usec of hrtimer slack */

    q->horizon = 10ULL * NSEC_PER_SEC; /* 10 seconds */
    q->horizon_drop = 1;               /* by default, drop packets beyond horizon */

    /* Default ce_threshold of 4294 seconds */
    q->ce_threshold = (u64)NSEC_PER_USEC * ~0U;

    qdisc_watchdog_init_clockid(&q->watchdog, sch, CLOCK_MONOTONIC);

    if (opt)
        err = marco_fq_change(sch, opt, extack);
    else
        err = marco_fq_resize(sch, q->fq_trees_log);

    return err;
}

static int marco_fq_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    u64 ce_threshold = q->ce_threshold;
    u64 horizon = q->horizon;
    struct nlattr *opts;

    opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
    if (opts == NULL)
        goto nla_put_failure;

    /* TCA_FQ_FLOW_DEFAULT_RATE is not used anymore */

    do_div(ce_threshold, NSEC_PER_USEC);
    do_div(horizon, NSEC_PER_USEC);

    if (nla_put_u32(skb, TCA_FQ_PLIMIT, sch->limit) ||
        nla_put_u32(skb, TCA_FQ_FLOW_PLIMIT, q->flow_plimit) ||
        nla_put_u32(skb, TCA_FQ_QUANTUM, q->quantum) ||
        nla_put_u32(skb, TCA_FQ_INITIAL_QUANTUM, q->initial_quantum) ||
        nla_put_u32(skb, TCA_FQ_RATE_ENABLE, q->rate_enable) ||
        nla_put_u32(skb, TCA_FQ_FLOW_MAX_RATE,
                    min_t(unsigned long, q->flow_max_rate, ~0U)) ||
        nla_put_u32(skb, TCA_FQ_FLOW_REFILL_DELAY,
                    jiffies_to_usecs(q->flow_refill_delay)) ||
        nla_put_u32(skb, TCA_FQ_ORPHAN_MASK, q->orphan_mask) ||
        nla_put_u32(skb, TCA_FQ_LOW_RATE_THRESHOLD,
                    q->low_rate_threshold) ||
        nla_put_u32(skb, TCA_FQ_CE_THRESHOLD, (u32)ce_threshold) ||
        nla_put_u32(skb, TCA_FQ_BUCKETS_LOG, q->fq_trees_log) ||
        nla_put_u32(skb, TCA_FQ_TIMER_SLACK, q->timer_slack) ||
        nla_put_u32(skb, TCA_FQ_HORIZON, (u32)horizon) ||
        nla_put_u8(skb, TCA_FQ_HORIZON_DROP, q->horizon_drop))
        goto nla_put_failure;

    return nla_nest_end(skb, opts);

nla_put_failure:
    return -1;
}

static int marco_fq_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
    struct marco_fq_sched_data *q = qdisc_priv(sch);
    struct tc_fq_qd_stats st;

    sch_tree_lock(sch);

    st.gc_flows = q->stat_gc_flows;
    st.highprio_packets = q->stat_internal_packets;
    st.tcp_retrans = 0;
    st.throttled = q->stat_throttled;
    st.flows_plimit = q->stat_flows_plimit;
    st.pkts_too_long = q->stat_pkts_too_long;
    st.allocation_errors = q->stat_allocation_errors;
    st.time_next_delayed_flow = q->time_next_delayed_flow + q->timer_slack -
                                ktime_get_ns();
    st.flows = q->flows;
    st.inactive_flows = q->inactive_flows;
    st.throttled_flows = q->throttled_flows;
    st.unthrottle_latency_ns = min_t(unsigned long,
                                     q->unthrottle_latency_ns, ~0U);
    st.ce_mark = q->stat_ce_mark;
    st.horizon_drops = q->stat_horizon_drops;
    st.horizon_caps = q->stat_horizon_caps;
    sch_tree_unlock(sch);

    return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc_ops fq_qdisc_ops __read_mostly = {
    .id = "marco_fq",
    .priv_size = sizeof(struct marco_fq_sched_data),

    .enqueue = marco_fq_enqueue,
    .dequeue = marco_fq_dequeue,
    .peek = qdisc_peek_dequeued,
    .init = marco_fq_init,
    .reset = marco_fq_reset,
    .destroy = marco_fq_destroy,
    .change = marco_fq_change,
    .dump = marco_fq_dump,
    .dump_stats = marco_fq_dump_stats,
    .owner = THIS_MODULE,
};

void clear_ip_count_table(void)
{
    struct hash_ip_count *ip_count;
    struct hlist_node *tmp;
    int bkt;

    hash_for_each_safe(ip_count_table, bkt, tmp, ip_count, hnode)
    {
        hash_del(&ip_count->hnode);
        kfree(ip_count);
    }
}

static int __init fq_module_init(void)
{
    printk("Load the marco fq_module");
    int ret;

    marco_fq_flow_cachep = kmem_cache_create("fq_flow_cache",
                                       sizeof(struct marco_fq_flow),
                                       0, 0, NULL);
    if (!marco_fq_flow_cachep)
        return -ENOMEM;

    ret = register_qdisc(&fq_qdisc_ops);
    if (ret)
        kmem_cache_destroy(marco_fq_flow_cachep);

    hash_init(ip_count_table);
    return ret;
}

static void __exit fq_module_exit(void)
{
    unregister_qdisc(&fq_qdisc_ops);
    kmem_cache_destroy(marco_fq_flow_cachep);
    clear_ip_count_table();
    printk("The marco_fq module unloaded");
}

module_init(fq_module_init)
    module_exit(fq_module_exit)
        MODULE_AUTHOR("Eric Dumazet");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fair Queue Packet Scheduler");
