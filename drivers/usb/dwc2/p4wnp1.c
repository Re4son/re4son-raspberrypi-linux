#include <net/sock.h>
#include <linux/netlink.h>
#include <net/genetlink.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>

#include "p4wnp1.h"





static uint32_t connect_state = 0xFFFFFFFF; //invalid state on start, to force a chqange on first update


/* Init the Tasklet by Static Method */
DECLARE_TASKLET(tasklet_connected, tasklet_fn_mcast_connection_state,1);
DECLARE_TASKLET(tasklet_disconnected, tasklet_fn_mcast_connection_state, 0);


/*
 *  Generic Netlink Family and Ops data
 */
// dummy method for P4WNP1_CMD_CONNECTION_STATE (only used in genl multicast)
static int get_connection_state(struct sk_buff *skb, struct genl_info *info) {
	return 0; //dummy
}

// policies describing the attribute (type checking)
static const struct nla_policy p4wnp1_policy[NUM_P4WNP1_ATTR] = {
	[P4WNP1_ATTR_CONNECTION_STATE] = { .type = NLA_U8, }, //uint8 type
};

// multicast groups
static const struct genl_multicast_group p4wnp1_mcgrps[] = {
		{ .name = P4WNP1_MULTICAST_GROUP_NAME_DWC2 },
};

// Operations for generic netlink family "P4wnP1"
static const struct genl_ops p4wnp1_ops[] = {
	{
		.cmd = P4WNP1_CMD_CONNECTION_STATE, // 0
		.flags = 0,
		.policy = p4wnp1_policy,
		.doit = get_connection_state, // will never be called, as we want to send events via multicast
		.dumpit = NULL,
	},
};


// Generic NL family definition
static struct genl_family family __ro_after_init = {
//	.id = GENL_ID_GENERATE,
	.name = P4WNP1_GENL_NAME,
	.hdrsize = 0,
	.version = 1,
	.maxattr = P4WNP1_ATTR_MAX,
	.netnsok = true,
	.module = THIS_MODULE,

	.ops = p4wnp1_ops,
	.n_ops = ARRAY_SIZE(p4wnp1_ops),
	.mcgrps = p4wnp1_mcgrps,
	.n_mcgrps = ARRAY_SIZE(p4wnp1_mcgrps),
};



/*Tasklet Function*/
void tasklet_fn_mcast_connection_state(unsigned long arg)
{
	u8 nl_state_report = 0;

        printk("Executing Tasklet Function : arg = %ld\n", arg);
	switch (arg) {
	case 0:
		nl_state_report = 0x00;
		printk("New USB Geadget connect state: %d\n", nl_state_report);    
		send_conn_state_genl_mcast(&nl_state_report);
		break;
	case 1:
		nl_state_report = 0x01;
		printk("New USB Geadget connect state: %d\n", nl_state_report);    
		send_conn_state_genl_mcast(&nl_state_report);
		break;
	}

}

// schedules a tasklet if state connection changed, tasklet sends genl mcast to dwc2 group
void update_con_state(uint32_t new_state) {
    if (new_state == connect_state) return; //ignore, no change
    
    connect_state = new_state;
    if (connect_state == 0x00001000) {
        tasklet_schedule(&tasklet_connected);
    } else {
        // nl_state_report = 0x00;
        tasklet_schedule(&tasklet_disconnected);
    }
}

// called by tasklet, 
void send_conn_state_genl_mcast(u8 *data)
{
    struct sk_buff *msg;
    void *genlmsghdr;
	int error;
	int flags = 0;

//    pr_alert("Sending netlink message\n");
    msg = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC); // GFP_KERNEL should be fine, too, as called in tasklet not in IRQ handler-
	if (!msg) {
		pr_err("genlmsg_new() failed.\n");
        return;
    }
    
//	pr_info("Putting message.\n");
	genlmsghdr = genlmsg_put(msg, 0, 0, &family, flags, P4WNP1_CMD_CONNECTION_STATE);
	if (!genlmsghdr) {
		pr_err("genlmsg_put() failed.\n");
		kfree_skb(msg);
		return;
	}
	
//	pr_info("Nla_putting integer.\n");
	error = nla_put_u8(msg, P4WNP1_ATTR_CONNECTION_STATE, *data);
	if (error) {
		pr_err("nla_put_u32() failed: %d\n", error);
		kfree_skb(msg);
		return;
	}

//	pr_info("Ending message.\n");
	genlmsg_end(msg, genlmsghdr);

//	pr_info("Multicasting message.\n");
//	pr_info("The group ID is %u.\n", family.mcgrp_offset);
	error = genlmsg_multicast_allns(&family, msg, 0, 0, GFP_KERNEL);
	if (error) {
		pr_err("genlmsg_multicast_allns() of dwc2 connection state failed: %d\n", error); //happens if no listener socket in userspace
		return;
	}
	pr_info("Success.\n");
    return;
}

int netlink_init(void) {
	int error;

	pr_info("Registering family.\n");
	error = genl_register_family(&family);
	if (error)
		pr_err("Family registration failed: %d\n", error);

	return error;	
}

void netlink_release(void)
{
	// Generic NL
	genl_unregister_family(&family);

	//stop tasklets
	tasklet_kill(&tasklet_connected);
	tasklet_kill(&tasklet_disconnected);
}
