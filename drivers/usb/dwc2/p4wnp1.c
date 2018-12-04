#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>

#include "p4wnp1.h"

#define MCAST_GROUP 24

static struct netlink_kernel_cfg nl_cfg_mcast = {0};
static struct sock *nl_sock_mcast = NULL;

static uint32_t connect_state = 0xFFFFFFFF; //invalid state on start, to force a chqange on first update

void update_con_state(uint32_t new_state) {
    u8 nl_state_report;
    if (new_state == connect_state) return; //ignore, no change
    
    connect_state = new_state;
    if (connect_state == 0x00001000)  nl_state_report = 0x01;
    else nl_state_report = 0x00;

    printk("New USB Geadget connect state: %d\n", nl_state_report);
    nl_mcast_send_data(&nl_state_report, 1);
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
        netlink_kernel_release(nl_sock_mcast);
    }
}

void nl_mcast_send_data(u8 *data, u32 len)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;

    pr_alert("Sending netlink message\n");
    skb = nlmsg_new(NLMSG_ALIGN(len), GFP_KERNEL);
    if (!skb)
    {
        pr_alert("dwc2 %s: Allocating space for netlink message failed\n", __func__);
        return;
    }

    nlh = nlmsg_put(skb, 0, 1, NLMSG_DONE, len, 0);
    memcpy(nlmsg_data(nlh), data, len);

    //nlmsg_multicast(nl_sock_mcast, skb, 0, MCAST_GROUP, GFP_KERNEL);
    nlmsg_multicast(nl_sock_mcast, skb, 0, MCAST_GROUP, GFP_ATOMIC); //called in an interrupt handler, use GFP_ATOMIC to avoid crash

    return;
}
