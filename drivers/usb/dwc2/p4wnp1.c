#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>


#include "p4wnp1.h"

#define MCAST_GROUP 24

static struct netlink_kernel_cfg nl_cfg_mcast = {0};
static struct sock *nl_sock_mcast = NULL;

static uint32_t connect_state = 0xFFFFFFFF; //invalid state on start, to force a chqange on first update


void nlmulticast_tasklet_fn(unsigned long); 
/* Init the Tasklet by Static Method */
DECLARE_TASKLET(tasklet_connected, nlmulticast_tasklet_fn,1);
DECLARE_TASKLET(tasklet_disconnected, nlmulticast_tasklet_fn, 0);
/*Tasklet Function*/
void nlmulticast_tasklet_fn(unsigned long arg)
{
	u8 nl_state_report = 0;

        printk("Executing Tasklet Function : arg = %ld\n", arg);
	switch (arg) {
	case 0:
		nl_state_report = 0x00;
                printk("New USB Geadget connect state: %d\n", nl_state_report);    
		nl_mcast_send_data(&nl_state_report, 1);
		break;
	case 1:
		nl_state_report = 0x01;
                printk("New USB Geadget connect state: %d\n", nl_state_report);    
		nl_mcast_send_data(&nl_state_report, 1);
		break;
	}

}

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

int netlink_init(void)
{
    /* MaMe82 init multicast netlink socket*/
    nl_cfg_mcast.flags = NL_CFG_F_NONROOT_RECV;
    nl_sock_mcast = netlink_kernel_create(&init_net, NETLINK_USERSOCK, &nl_cfg_mcast);
    //nl_sock_mcast = netlink_kernel_create(&init_net, NETLINK_KOBJECT_UEVENT, &nl_cfg_mcast);

    if (!nl_sock_mcast)
    {
        pr_alert("MaMe82: %s: Error creating netlink socket for multicast\n", __FUNCTION__);
        return -1;
    }

    return 0;
}

void netlink_release(void)
{
    if (nl_sock_mcast != NULL)
    {
	tasklet_kill(&tasklet_connected);
	tasklet_kill(&tasklet_disconnected);
        netlink_kernel_release(nl_sock_mcast);
    }
}

void nl_mcast_send_data(u8 *data, u32 len)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;

    if (nl_sock_mcast == NULL) {
	pr_alert("Not sending netlink message, socket not initialized\n");
	return;
    }

    pr_alert("Sending netlink message\n");
    skb = nlmsg_new(NLMSG_ALIGN(len+1), GFP_ATOMIC);
    if (!skb)
    {
        pr_alert("dwc2 %s: Allocating space for netlink message failed\n", __func__);
        return;
    }

    nlh = nlmsg_put(skb, 0, 1, NLMSG_DONE, len+1, 0);
    memcpy(nlmsg_data(nlh), data, len);

    //calls to nlmsg_multicast or netlink_broadcast use netlink_broadcast_filtered-->do_one_broadcast-->peernet2id
    //peernet2id--> spin_unlock_bh-->raw_spin_lock_bh-->_raw_spin_unlock_bh-->__raw_spin_unlock_bh-->__local_bh_enable_ip
    //__local_bh_enable_ip warns if called in (in_irq() || irqs_disabled) 
    

    //nlmsg_multicast(nl_sock_mcast, skb, 0, MCAST_GROUP, GFP_KERNEL);
    //nlmsg_multicast(nl_sock_mcast, skb, 0, MCAST_GROUP, GFP_ATOMIC); //called in an interrupt handler, use GFP_ATOMIC to avoid crash
    netlink_broadcast(nl_sock_mcast, skb, 0, MCAST_GROUP, GFP_ATOMIC); //called in an interrupt handler, use GFP_ATOMIC to avoid crash

    return;
}
