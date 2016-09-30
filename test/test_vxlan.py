#!/usr/bin/env python

import unittest
from framework import VppTestCase, VppTestRunner
from util import Util
from template_bd import BridgeDomain

from scapy.layers.l2 import Ether
from scapy.layers.inet import IP, UDP
from scapy_handlers.vxlan import VXLAN


## VXLAN Test cases using bridge domain to test encapsulation and decapsulation
class TestVxlan(BridgeDomain, Util, VppTestCase):
    """ VXLAN Test Case """

    ## Run __init__ for all parent class
    #
    #  Initialize BridgeDomain objects, set documentation string for inherited
    #  tests and initialize VppTestCase object which must be called after
    #  doc strings are set.
    def __init__(self, *args):
        BridgeDomain.__init__(self)
        self.test_decap.__func__.__doc__ = ' Decapsulate path to BD '
        self.test_encap.__func__.__doc__ = ' Encapsulate path to BD '
        VppTestCase.__init__(self, *args)

    ## Implementation for BridgeDomain encapsulate function.
    #
    #  Encapsulation is done by adding original frame into new Ethernet IP UDP with VXLAN header.
    def encapsulate(self, pkt):
        return (Ether(src=self.MY_MACS[0], dst=self.VPP_MACS[0]) /
                IP(src=self.MY_IP4S[0], dst=self.VPP_IP4S[0]) /
                UDP(sport=4789, dport=4789, chksum=0) /
                VXLAN(vni=1) /
                pkt)

    ## Implementation for BridgeDomain decapsulate function.
    #
    #  Encapsulation is done by adding original frame into new Ethernet IP UDP with VXLAN header.
    def decapsulate(self, pkt):
        return pkt[VXLAN].payload

    ## Implementation for BridgeDomain check_encapsulation function.
    #
    def check_encapsulation(self, pkt):
        # TODO: add error messages
        ## Check Ethernet header source must be VPP's MAC and destination MY MAC which VPP knows because ARP in setup.
        self.assertEqual(pkt[Ether].src, self.VPP_MACS[0])
        self.assertEqual(pkt[Ether].dst, self.MY_MACS[0])
        ## Check tunnel source(VPP IP) and destination(MY IP).
        self.assertEqual(pkt[IP].src, self.VPP_IP4S[0])
        self.assertEqual(pkt[IP].dst, self.MY_IP4S[0])
        ## Check UDP destination port, source could be arbitrary.
        self.assertEqual(pkt[UDP].dport, 4789)
        # TODO: checksum check
        ## Check VNI, based on configuration must be 1.
        self.assertEqual(pkt[VXLAN].vni, 1)

    ## Prepare VXLAN test environment.
    @classmethod
    def setUpClass(cls):
        super(TestVxlan, cls).setUpClass()
        try:
            ## Create 2 pg interfaces.
            cls.create_interfaces(range(2))
            ## Configure IPv4 addressing on pg0.
            cls.config_ip4([0])
            ## Determine MAC address for VPP's IP.
            cls.resolve_arp([0])

            ## Create VXLAN VTEP on pg0, and put vxlan_tunnel0 and pg1 into BD.
            cls.api("vxlan_add_del_tunnel src %s dst %s vni 1" %
                    (cls.VPP_IP4S[0], cls.MY_IP4S[0]))
            cls.api("sw_interface_set_l2_bridge vxlan_tunnel0 bd_id 1")
            cls.api("sw_interface_set_l2_bridge pg1 bd_id 1")
        except:
            ## In case setUpClass fails run tear down (it's not called automaticly)
            cls.tearDownClass()
            raise

    ## Tear down which is called after each test_* in method in this object.
    def tearDown(self):
        super(TestVxlan, self).tearDown()
        self.cli(2, "show bridge-domain 1 detail")

if __name__ == '__main__':
    unittest.main(testRunner=VppTestRunner)
