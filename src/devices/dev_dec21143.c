/*
 *  Copyright (C) 2005-2009  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright  
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *   
 *
 *  COMMENT: DEC 21143 "Tulip" ethernet controller
 *
 *  Implemented from Intel document 278074-001 ("21143 PC/CardBus 10/100Mb/s
 *  Ethernet LAN Controller") and by reverse-engineering OpenBSD and NetBSD
 *  sources.
 *
 *  This device emulates several sub-components:
 *
 *	21143:	This is the actual ethernet controller.
 *
 *	MII:	The "physical" network interface.
 *
 *	SROM:	A ROM area containing setting such as which MAC address to
 *		use, and info about the MII.
 *
 *
 *  TODO:
 *	o)  Handle _writes_ to MII registers.
 *	o)  Make it work with modern Linux kernels (as a guest OS).
 *	o)  Endianness for descriptors? If necessary.
 *	o)  Don't hardcode as many values.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "emul.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "net.h"

#include "thirdparty/mii.h"
#include "thirdparty/tulipreg.h"


/*  #define debug fatal  */

#define	DEC21143_TICK_SHIFT		16

#define	N_REGS			32
#define	ROM_WIDTH		6

struct dec21143_data {
	/*  NIC common data  */
	struct nic_data	nic;

	struct interrupt irq;
	int		irq_was_asserted;

	/*  PCI:  */
	int		pci_little_endian;

	/*  SROM emulation:  */
	uint8_t		srom[1 << (ROM_WIDTH + 1)];
	int		srom_curbit;
	int		srom_opcode;
	int		srom_opcode_has_started;
	int		srom_addr;

	/*  MII PHY emulation:  */
	uint16_t	mii_phy_reg[MII_NPHY * 32];
	int		mii_state;
	int		mii_bit;
	int		mii_opcode;
	int		mii_phyaddr;
	int		mii_regaddr;

	/*  21143 registers:  */
	uint32_t	reg[N_REGS];

	/*  Internal TX state:  */
	uint32_t	cur_tx_addr;
	unsigned char	*cur_tx_buf;
	int		cur_tx_buf_len;
	int		tx_idling;
	int		tx_idling_threshold;

	/*  Internal RX state:  */
	uint32_t	cur_rx_addr;
	unsigned char	*cur_rx_buf;
	int		cur_rx_buf_len;
	int		cur_rx_offset;

	/*
	 *  Receive filter information.  We keep our own copy of
	 *  the promiscuous flag because to implement some of the
	 *  filtering modes, we need to tell the network layer that
	 *  we want all packets.
	 */
	int		(*drop_packet)(struct net *, struct dec21143_data *);
	int		allmulti;
	int		promiscuous;
	int		filter_needs_promiscuous;
	uint8_t		perfect_filter[6 * TULIP_MAXADDRS];

	/*  Only 16 bits are used per filter word.  */
#define	MCHASH_NWORDS	(TULIP_MCHASHSIZE / 16)
	uint32_t	hash_filter[MCHASH_NWORDS];
	int		hash_filter_saturated;

	/*
	 *  XXX XXX XXX
	 *  XXX UGLY HACK.  Need a proper way to deal with
	 *  XXX different PCI vs. CPU views of RAM.
	 *  XXX XXX XXX
	 */
	uint32_t	xxx_dma_to_phys_mask;
};

/*  XXX This is an UGLY hack.  */
static uint64_t dma_to_phys(const struct dec21143_data *d, uint32_t dma_addr)
{
	return dma_addr & d->xxx_dma_to_phys_mask;
}


static inline uint32_t load_le32(const uint8_t *buf)
{
	return buf[0] | ((uint32_t)buf[1] << 8) |
	    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}


static inline void store_le32(uint8_t *buf, uint32_t val)
{
	buf[0] = (uint8_t)val;
	buf[1] = (uint8_t)(val >> 8);
	buf[2] = (uint8_t)(val >> 16);
	buf[3] = (uint8_t)(val >> 24);
}


/*  Internal states during MII data stream decode:  */
#define	MII_STATE_RESET				0
#define	MII_STATE_START_WAIT			1
#define	MII_STATE_READ_OP			2
#define	MII_STATE_READ_PHYADDR_REGADDR		3
#define	MII_STATE_A				4
#define	MII_STATE_D				5
#define	MII_STATE_IDLE				6


/*
 * The 21143 has multiple address matching modes:
 *
 *	- Perfect Filtering: The chip interprets the descriptor buffer
 *	  as a table of 16 MAC addresses that it should match.  The
 *	  station address and broadcast must be included in the list.
 *
 *	- Hash Perfect Filtering: The chip interprets the descriptor buffer
 *	  as a 512-bit hash table plus one perfect filter match.  Multicast
 *	  addresses only are matched against the hash table.
 *
 *	- Inverse Filtering: Like Perfect Filtering, but the table is
 *	  addresses NOT to match.
 *
 *	- Hash-only Filtering: Like Hash Perfect, except without the Perfect.
 *	  All addresses are matched against the hash table.
 *
 * The mode seleted by the TDCTL descriptor field is reflected in 3
 * read-only bits in the OPMODE register.
 *
 * We implement all 4 (NetBSD, at least, is known to use Perfect and
 * Hash Perfect on the 21143; it also uses Hash-only on the 21140).
 */

#define	TDCTL_Tx_FT_MASK	(TDCTL_Tx_FT1|TDCTL_Tx_FT0)

#define	dec21143_mchash(addr)	\
	(net_ether_crc32_le((addr), 6) & (TULIP_MCHASHSIZE - 1))

static int dec21143_drop_packet_perfect(struct net *net,
	struct dec21143_data *d)
{
	int i;

	for (i = 0; i < TULIP_MAXADDRS; i++) {
		if (net_ether_eq(d->cur_rx_buf, &d->perfect_filter[6 * i])) {
			/* Match! */
			return 0;
		}
	}

	return 1;
}

static int dec21143_drop_packet_hashperfect(struct net *net,
	struct dec21143_data *d)
{

	/*
	 * In this mode, we have the network layer match our station
	 * address, and we reflect the true promiscuous status there
	 * as well.  This means that if it's not a multicast address,
	 * then it's already been sufficiently matched.
	 */
	if (! net_ether_multicast(d->cur_rx_buf))
		return 0;

	/*
	 * Note that the broadcast address is also checked against
	 * the hash table in this mode!
	 */

	const uint32_t hash = dec21143_mchash(d->cur_rx_buf);
	if (d->hash_filter[hash >> 4] & (1U << (hash & 0xf))) {
		/* Match! */
		return 0;
	}

	return 1;
}

static int dec21143_drop_packet_inverse(struct net *net,
	struct dec21143_data *d)
{
	return !dec21143_drop_packet_perfect(net, d);
}

static int dec21143_drop_packet_hashonly(struct net *net,
	struct dec21143_data *d)
{
	const uint32_t hash = dec21143_mchash(d->cur_rx_buf);
	if (d->hash_filter[hash >> 4] & (1U << (hash & 0xf))) {
		/* Match! */
		return 0;
	}

	return 1;
}


/*
 *  dec21143_rx_drop_packet():
 *
 *  Implement the logic to determine if we should drop a packet
 *  before paassing it to the guest.  Returns 1 if the packet
 *  was dropped.
 */
static int dec21143_rx_drop_packet(struct net *net, struct dec21143_data *d)
{
	/* Only implement filtering if using a tap device. */
	if (net->tapdev == NULL)
		return 0;

	/*
	 * We might have told the network layer that we're promiscuous
	 * due to the chosen filtering mode, so check the truth here.
	 */
	if (d->promiscuous)
		return 0;

	/*
	 * If the guest wants all multicast (either all the bits are
	 * set or the OPMODE_PM bit is set), then check to see if we
	 * can short-circuit the checks.
	 */
	if (d->allmulti && net_ether_multicast(d->cur_rx_buf))
		return 0;

	/*
	 * Note that if we haven't gotten a setup packet yet, then
	 * d->drop_packet will be NULL.  If this happens, we always
	 * drop.  This is akin to the real hardware defaulting to
	 * Perfect filtering mode but not having any valid addresses
	 * in the list to match against.
	 */
	if (d->drop_packet == NULL || d->drop_packet(net, d)) {
		/* Not for us; drop the packet. */
		free(d->cur_rx_buf);
		d->cur_rx_buf = NULL;
		d->cur_rx_buf_len = 0;
		return 1;
	}

	return 0;
}


/*
 *  dec21143_update_rx_mode():
 *
 *  Update promiscuous / allmulti indicators based on OPMODE
 *  and filter state.
 */
static void dec21143_update_rx_mode(struct dec21143_data *d)
{
	int opmode_pr = (d->reg[CSR_OPMODE / 8] & OPMODE_PR) != 0;
	int opmode_pm = (d->reg[CSR_OPMODE / 8] & OPMODE_PM) != 0;

	debug("[ dec21143 rx mode: opmode_pr = %d                ]\n",
	      opmode_pr);
	debug("[ dec21143 rx mode: filter_needs_promiscuous = %d ]\n",
	      d->filter_needs_promiscuous);
	debug("[ dec21143 rx mode: opmode_pm = %d                ]\n",
	      opmode_pm);
	debug("[ dec21143 rx mode: filter_saturated = %d         ]\n",
	      d->hash_filter_saturated);

	d->promiscuous = opmode_pr;
	d->nic.promiscuous_mode =
	    d->promiscuous || d->filter_needs_promiscuous;

	d->allmulti = opmode_pm || d->hash_filter_saturated;
}


/*
 *  dec21143_rx():
 *
 *  Receive a packet. (If there is no current packet, then check for newly
 *  arrived ones. If the current packet couldn't be fully transfered the
 *  last time, then continue on that packet.)
 */
int dec21143_rx(struct cpu *cpu, struct dec21143_data *d)
{
	uint32_t addr = d->cur_rx_addr, bufaddr;
	unsigned char descr[16];
	uint32_t rdes0, rdes1, rdes2, rdes3;
	int bufsize, buf1_size, buf2_size, i, writeback_len = 4, to_xfer;

	/*  No current packet? Then check for new ones.  */
	while (d->cur_rx_buf == NULL) {
		/*  Nothing available? Then abort.  */
		if (!net_ethernet_rx_avail(d->nic.net, &d->nic))
			return 0;

		/*  Get the next packet into our buffer:  */
		net_ethernet_rx(d->nic.net, &d->nic,
		    &d->cur_rx_buf, &d->cur_rx_buf_len);

		if (dec21143_rx_drop_packet(d->nic.net, d))
			continue;

		/*  Append a 4 byte CRC:  */
		d->cur_rx_buf_len += 4;
		CHECK_ALLOCATION(d->cur_rx_buf = (unsigned char *) realloc(d->cur_rx_buf,
		    d->cur_rx_buf_len));

		/*  Well... the CRC is just zeros, for now.  */
		memset(d->cur_rx_buf + d->cur_rx_buf_len - 4, 0, 4);

		d->cur_rx_offset = 0;
	}

	/*  fatal("{ dec21143_rx: base = 0x%08x }\n", (int)addr);  */

	if (!cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, addr),
	    descr, sizeof(uint32_t), MEM_READ, PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_rx: memory_rw failed! ]\n");
		return 0;
	}

	rdes0 = load_le32(&descr[0]);

	/*  Only use descriptors owned by the 21143:  */
	if (!(rdes0 & TDSTAT_OWN)) {
		d->reg[CSR_STATUS/8] |= STATUS_RU;
		return 0;
	}

	if (!cpu->memory_rw(cpu, cpu->mem,
	    dma_to_phys(d, addr + sizeof(uint32_t)),
	    descr + sizeof(uint32_t), sizeof(uint32_t) * 3, MEM_READ,
	    PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_rx: memory_rw failed! ]\n");
		return 0;
	}

	rdes1 = load_le32(&descr[4]);
	rdes2 = load_le32(&descr[8]);
	rdes3 = load_le32(&descr[12]);

	buf1_size = rdes1 & TDCTL_SIZE1;
	buf2_size = (rdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;
	bufaddr = buf1_size? rdes2 : rdes3;
	bufsize = buf1_size? buf1_size : buf2_size;

	d->reg[CSR_STATUS/8] &= ~STATUS_RS;

	if (rdes1 & TDCTL_ER)
		d->cur_rx_addr = d->reg[CSR_RXLIST / 8];
	else {
		if (rdes1 & TDCTL_CH)
			d->cur_rx_addr = rdes3;
		else
			d->cur_rx_addr += 4 * sizeof(uint32_t);
	}

	debug("{ RX (%llx): 0x%08x 0x%08x 0x%x 0x%x: buf %i bytes at 0x%x }\n",
	    (long long)addr, rdes0, rdes1, rdes2, rdes3, bufsize, (int)bufaddr);

	/*  Turn off all status bits, and give up ownership:  */
	rdes0 = 0x00000000;

	to_xfer = d->cur_rx_buf_len - d->cur_rx_offset;
	if (to_xfer > bufsize)
		to_xfer = bufsize;

	/*  DMA bytes from the packet into emulated physical memory:  */
	for (i=0; i<to_xfer; i++) {
		cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, bufaddr + i),
		    d->cur_rx_buf + d->cur_rx_offset + i, 1, MEM_WRITE,
		    PHYSICAL | NO_EXCEPTIONS);
		/*  fatal(" %02x", d->cur_rx_buf[d->cur_rx_offset + i]);  */
	}

	/*  Was this the first buffer in a frame? Then mark it as such.  */
	if (d->cur_rx_offset == 0)
		rdes0 |= TDSTAT_Rx_FS;

	d->cur_rx_offset += to_xfer;

	/*  Frame completed?  */
	if (d->cur_rx_offset >= d->cur_rx_buf_len) {
		rdes0 |= TDSTAT_Rx_LS;

		/*  Set the frame length:  */
		rdes0 |= (d->cur_rx_buf_len << 16) & TDSTAT_Rx_FL;

		/*  Frame too long? (1518 is max ethernet frame length)  */
		if (d->cur_rx_buf_len > 1518)
			rdes0 |= TDSTAT_Rx_TL;

		/*  Cause a receiver interrupt:  */
		d->reg[CSR_STATUS/8] |= STATUS_RI;

		free(d->cur_rx_buf);
		d->cur_rx_buf = NULL;
		d->cur_rx_buf_len = 0;
	}

	/*  Descriptor writeback:  */
	store_le32(&descr[0], rdes0);
	if (writeback_len > 1) {
		store_le32(&descr[4], rdes1);
		store_le32(&descr[8], rdes2);
		store_le32(&descr[12], rdes3);
	}

	if (!cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, addr), descr,
	    sizeof(uint32_t) * writeback_len, MEM_WRITE,
	    PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_rx: memory_rw failed! ]\n");
		return 0;
	}

	return 1;
}


/*
 *  dec21143_setup_copy_enaddr():
 *
 *  Copy an Ethernet address out of the setup packet.
 */
static void dec21143_setup_copy_enaddr(uint8_t *enaddr,
	const uint32_t *setup_packet)
{
	int i;

	for (i = 0; i < 3; i++) {
		enaddr[i*2    ] = (uint8_t)setup_packet[i];
		enaddr[i*2 + 1] = (uint8_t)(setup_packet[i] >> 8);
	}
}


/*
 *  dec21143_setup_perfect():
 *
 *  Setup perfect filtering mode.
 */
static void dec21143_setup_perfect(struct dec21143_data *d,
	const uint32_t *setup_packet)
{
	int i;

	for (i = 0; i < TULIP_MAXADDRS; i++) {
		dec21143_setup_copy_enaddr(&d->perfect_filter[i * 6],
		    &setup_packet[i * 3]);
		debug("dec21143 PERFECT[%d] %02x:%02x:%02x:%02x:%02x:%02x\n",
		    i,
		    d->perfect_filter[i*6 + 0],
		    d->perfect_filter[i*6 + 1],
		    d->perfect_filter[i*6 + 2],
		    d->perfect_filter[i*6 + 3],
		    d->perfect_filter[i*6 + 4],
		    d->perfect_filter[i*6 + 5]);
	}

	d->drop_packet = dec21143_drop_packet_perfect;
}


/*
 *  dec21143_setup_hashperfect():
 *
 *  Setup hash-perfect filtering mode.
 */
static void dec21143_setup_hashperfect(struct dec21143_data *d,
	const uint32_t *setup_packet)
{
	int i;

	debug("dec21143 HASHPERFECT:");
	for (i = 0; i < MCHASH_NWORDS; i++) {
		if ((i % 8) == 0)
			debug("\n\t");
		debug(" %04x", setup_packet[i]);
		d->hash_filter[i] = setup_packet[i];
		d->hash_filter_saturated |= (d->hash_filter[i] == 0xffff);
	}
	debug("\n");

	dec21143_setup_copy_enaddr(d->nic.mac_address, &setup_packet[39]);
	debug("dec21143 HASHPERFECT %02x:%02x:%02x:%02x:%02x:%02x\n",
	      d->nic.mac_address[0],
	      d->nic.mac_address[1],
	      d->nic.mac_address[2],
	      d->nic.mac_address[3],
	      d->nic.mac_address[4],
	      d->nic.mac_address[5]);

	d->filter_needs_promiscuous = 0;
	d->drop_packet = dec21143_drop_packet_hashperfect;
}


/*
 *  dec21143_setup_inverse():
 *
 *  Setup inverse filtering mode.
 */
static void dec21143_setup_inverse(struct dec21143_data *d,
	const uint32_t *setup_packet)
{
	dec21143_setup_perfect(d, setup_packet);
	debug("dec21143 INVERSE ^^^^\n");
	d->drop_packet = dec21143_drop_packet_inverse;
}


/*
 *  dec21143_setup_hashonly():
 *
 *  Setup hash-only filtering mode.
 */
static void dec21143_setup_hashonly(struct dec21143_data *d,
	const uint32_t *setup_packet)
{
	int i;

	debug("dec21143 HASHONLY:");
	for (i = 0; i < MCHASH_NWORDS; i++) {
		if ((i % 8) == 0)
			fatal("\n\t");
		debug(" %04x", setup_packet[i]);
		d->hash_filter[i] = setup_packet[i];
		d->hash_filter_saturated |= (d->hash_filter[i] == 0xffff);
	}
	debug("\n");

	d->drop_packet = dec21143_drop_packet_hashonly;
}


/*
 *  dec21143_process_setup_packet():
 *
 *  Process the address filter setup packet.
 */
static void dec21143_process_setup_packet(struct cpu *cpu,
	struct dec21143_data *d, uint32_t tdctl, uint32_t bufaddr)
{
	uint32_t setup_packet[TULIP_SETUP_PACKET_LEN / sizeof(uint32_t)];
	uint8_t *cp = (uint8_t *)setup_packet;
	uint32_t tmp;
	int i;

	if (!cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, bufaddr), cp,
	    TULIP_SETUP_PACKET_LEN, MEM_READ, PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_process_setup_packet: memory_rw failed! ]\n");
		return;
	}

	/* Ensure host order of each word. */
	for (i = 0; i < TULIP_SETUP_PACKET_LEN; i += sizeof(uint32_t)) {
		tmp = load_le32(&cp[i]);
		setup_packet[i / sizeof(uint32_t)] = tmp;
	}

	/* Defaults. */
	d->hash_filter_saturated = 0;
	d->filter_needs_promiscuous = 1;

	d->reg[CSR_OPMODE / 8] &= ~(OPMODE_HP | OPMODE_HO | OPMODE_IF);

	switch (tdctl & TDCTL_Tx_FT_MASK) {
	case TDCTL_Tx_FT_PERFECT:
		dec21143_setup_perfect(d, setup_packet);
		break;

	case TDCTL_Tx_FT_HASH:
		dec21143_setup_hashperfect(d, setup_packet);
		d->reg[CSR_OPMODE / 8] |= OPMODE_HP;
		break;

	case TDCTL_Tx_FT_INVERSE:
		dec21143_setup_inverse(d, setup_packet);
		d->reg[CSR_OPMODE / 8] |= OPMODE_IF;
		break;

	case TDCTL_Tx_FT_HASHONLY:
		dec21143_setup_hashonly(d, setup_packet);
		d->reg[CSR_OPMODE / 8] |= OPMODE_HO;
		break;
	}

	dec21143_update_rx_mode(d);
}


/*
 *  dec21143_tx():
 *
 *  Transmit a packet, if the guest OS has marked a descriptor as containing
 *  data to transmit.
 */
int dec21143_tx(struct cpu *cpu, struct dec21143_data *d)
{
	uint32_t addr = d->cur_tx_addr, bufaddr;
	unsigned char descr[16];
	uint32_t tdes0, tdes1, tdes2, tdes3;
	int bufsize, buf1_size, buf2_size, i;

	if (!cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, addr), descr,
	    sizeof(uint32_t), MEM_READ, PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_tx: memory_rw failed! ]\n");
		return 0;
	}

	tdes0 = load_le32(&descr[0]);

	/*  fatal("{ dec21143_tx: base=0x%08x, tdes0=0x%08x }\n",
	    (int)addr, (int)tdes0);  */

	/*  Only process packets owned by the 21143:  */
	if (!(tdes0 & TDSTAT_OWN)) {
		if (d->tx_idling > d->tx_idling_threshold) {
			d->reg[CSR_STATUS/8] |= STATUS_TU;
			d->tx_idling = 0;
		} else
			d->tx_idling ++;
		return 0;
	}

	if (!cpu->memory_rw(cpu, cpu->mem,
	    dma_to_phys(d, addr + sizeof(uint32_t)),
	    descr + sizeof(uint32_t), sizeof(uint32_t) * 3, MEM_READ,
	    PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_tx: memory_rw failed! ]\n");
		return 0;
	}

	tdes1 = load_le32(&descr[4]);
	tdes2 = load_le32(&descr[8]);
	tdes3 = load_le32(&descr[12]);

	buf1_size = tdes1 & TDCTL_SIZE1;
	buf2_size = (tdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;
	bufaddr = buf1_size? tdes2 : tdes3;
	bufsize = buf1_size? buf1_size : buf2_size;

	d->reg[CSR_STATUS/8] &= ~STATUS_TS;

	if (tdes1 & TDCTL_ER)
		d->cur_tx_addr = d->reg[CSR_TXLIST / 8];
	else {
		if (tdes1 & TDCTL_CH)
			d->cur_tx_addr = tdes3;
		else
			d->cur_tx_addr += 4 * sizeof(uint32_t);
	}

	/*
	fatal("{ TX (%x): 0x%08x 0x%08x 0x%x 0x%x: buf %i bytes at 0x%x }\n",
	  addr, tdes0, tdes1, tdes2, tdes3, bufsize, bufaddr);
	*/

	/*  Assume no error:  */
	tdes0 &= ~ (TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC
	    | TDSTAT_Tx_NC | TDSTAT_Tx_LO | TDSTAT_Tx_TO | TDSTAT_ES);

	if (tdes1 & TDCTL_Tx_SET) {
		/*
		 *  Setup Packet.
		 */
		/*  fatal("{ TX: setup packet }\n");  */
		if (bufsize != TULIP_SETUP_PACKET_LEN)
			fatal("[ dec21143: setup packet len = %i, should be"
			    " %d! ]\n", (int)bufsize, TULIP_SETUP_PACKET_LEN);
		else
			dec21143_process_setup_packet(cpu, d, tdes1, bufaddr);
		if (tdes1 & TDCTL_Tx_IC)
			d->reg[CSR_STATUS/8] |= STATUS_TI;
		/*  New descriptor values, according to the docs:  */
		tdes0 = 0x7fffffff; tdes1 = 0xffffffff;
		tdes2 = 0xffffffff; tdes3 = 0xffffffff;
	} else {
		/*
		 *  Data Packet.
		 */
		/*  fatal("{ TX: data packet: ");  */
		if (tdes1 & TDCTL_Tx_FS) {
			/*  First segment. Let's allocate a new buffer:  */
			/*  fatal("new frame }\n");  */

			CHECK_ALLOCATION(d->cur_tx_buf = (unsigned char *) malloc(bufsize));
			d->cur_tx_buf_len = 0;
		} else {
			/*  Not first segment. Increase the length of
			    the current buffer:  */
			/*  fatal("continuing last frame }\n");  */

			if (d->cur_tx_buf == NULL)
				fatal("[ dec21143: WARNING! tx: middle "
				    "segment, but no first segment?! ]\n");

			CHECK_ALLOCATION(d->cur_tx_buf = (unsigned char *) realloc(d->cur_tx_buf,
			    d->cur_tx_buf_len + bufsize));
		}

		/*  "DMA" data from emulated physical memory into the buf:  */
		for (i=0; i<bufsize; i++) {
			cpu->memory_rw(cpu, cpu->mem,
			    dma_to_phys(d, bufaddr + i),
			    d->cur_tx_buf + d->cur_tx_buf_len + i, 1, MEM_READ,
			    PHYSICAL | NO_EXCEPTIONS);
			/*  fatal(" %02x", d->cur_tx_buf[
			    d->cur_tx_buf_len + i]);  */
		}

		d->cur_tx_buf_len += bufsize;

		/*  Last segment? Then actually transmit it:  */
		if (tdes1 & TDCTL_Tx_LS) {
			/*  fatal("{ TX: data frame complete. }\n");  */
			if (d->nic.net != NULL) {
				net_ethernet_tx(d->nic.net, &d->nic,
				    d->cur_tx_buf, d->cur_tx_buf_len);
			} else {
				static int warn = 0;
				if (!warn)
					fatal("[ dec21143: WARNING! Not "
					    "connected to a network! ]\n");
				warn = 1;
			}

			free(d->cur_tx_buf);
			d->cur_tx_buf = NULL;
			d->cur_tx_buf_len = 0;

			/*  Interrupt, if Tx_IC is set:  */
			if (tdes1 & TDCTL_Tx_IC)
				d->reg[CSR_STATUS/8] |= STATUS_TI;
		}

		/*  We are done with this segment.  */
		tdes0 &= ~TDSTAT_OWN;
	}

	/*  Error summary:  */
	if (tdes0 & (TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC
	    | TDSTAT_Tx_NC | TDSTAT_Tx_LO | TDSTAT_Tx_TO))
		tdes0 |= TDSTAT_ES;

	/*  Descriptor writeback:  */
	store_le32(&descr[0], tdes0);
	store_le32(&descr[4], tdes1);
	store_le32(&descr[8], tdes2);
	store_le32(&descr[12], tdes3);

	if (!cpu->memory_rw(cpu, cpu->mem, dma_to_phys(d, addr), descr,
	    sizeof(uint32_t) * 4, MEM_WRITE, PHYSICAL | NO_EXCEPTIONS)) {
		fatal("[ dec21143_tx: memory_rw failed! ]\n");
		return 0;
	}

	return 1;
}


DEVICE_TICK(dec21143)
{
	struct dec21143_data *d = (struct dec21143_data *) extra;
	int asserted;

	if (d->reg[CSR_OPMODE / 8] & OPMODE_ST)
		while (dec21143_tx(cpu, d))
			;

	if (d->reg[CSR_OPMODE / 8] & OPMODE_SR)
		while (dec21143_rx(cpu, d))
			;

	/*  Normal and Abnormal interrupt summary:  */
	d->reg[CSR_STATUS / 8] &= ~(STATUS_NIS | STATUS_AIS);
	if (d->reg[CSR_STATUS / 8] & 0x00004845)
		d->reg[CSR_STATUS / 8] |= STATUS_NIS;
	if (d->reg[CSR_STATUS / 8] & 0x0c0037ba)
		d->reg[CSR_STATUS / 8] |= STATUS_AIS;

	asserted = d->reg[CSR_STATUS / 8] & d->reg[CSR_INTEN / 8] & 0x0c01ffff;

	if (asserted)
		INTERRUPT_ASSERT(d->irq);
	if (!asserted && d->irq_was_asserted)
		INTERRUPT_DEASSERT(d->irq);

	/*  Remember assertion flag:  */
	d->irq_was_asserted = asserted;
}


/*
 *  mii_access():
 *
 *  This function handles accesses to the MII. Data streams seem to be of the
 *  following format:
 *
 *      vv---- starting delimiter
 *  ... 01 xx yyyyy zzzzz a[a] dddddddddddddddd
 *         ^---- I am starting with mii_bit = 0 here
 *
 *  where x = opcode (10 = read, 01 = write)
 *        y = PHY address
 *        z = register address
 *        a = on Reads: ACK bit (returned, should be 0)
 *            on Writes: _TWO_ dummy bits (10)
 *        d = 16 bits of data (MSB first)
 */
static void mii_access(struct cpu *cpu, struct dec21143_data *d,
	uint32_t oldreg, uint32_t idata)
{
	int obit, ibit = 0;
	uint16_t tmp;

	/*  Only care about data during clock cycles:  */
	if (!(idata & MIIROM_MDC))
		return;

	if (idata & MIIROM_MDC && oldreg & MIIROM_MDC)
		return;

	/*  fatal("[ mii_access(): 0x%08x ]\n", (int)idata);  */

	if (idata & MIIROM_BR) {
		fatal("[ mii_access(): MIIROM_BR: TODO ]\n");
		return;
	}

	obit = idata & MIIROM_MDO? 1 : 0;

	if (d->mii_state >= MII_STATE_START_WAIT &&
	    d->mii_state <= MII_STATE_READ_PHYADDR_REGADDR &&
	    idata & MIIROM_MIIDIR)
		fatal("[ mii_access(): bad dir? ]\n");

	switch (d->mii_state) {

	case MII_STATE_RESET:
		/*  Wait for a starting delimiter (0 followed by 1).  */
		if (obit)
			return;
		if (idata & MIIROM_MIIDIR)
			return;
		/*  fatal("[ mii_access(): got a 0 delimiter ]\n");  */
		d->mii_state = MII_STATE_START_WAIT;
		d->mii_opcode = 0;
		d->mii_phyaddr = 0;
		d->mii_regaddr = 0;
		break;

	case MII_STATE_START_WAIT:
		/*  Wait for a starting delimiter (0 followed by 1).  */
		if (!obit)
			return;
		if (idata & MIIROM_MIIDIR) {
			d->mii_state = MII_STATE_RESET;
			return;
		}
		/*  fatal("[ mii_access(): got a 1 delimiter ]\n");  */
		d->mii_state = MII_STATE_READ_OP;
		d->mii_bit = 0;
		break;

	case MII_STATE_READ_OP:
		if (d->mii_bit == 0) {
			d->mii_opcode = obit << 1;
			/*  fatal("[ mii_access(): got first opcode bit "
			    "(%i) ]\n", obit);  */
		} else {
			d->mii_opcode |= obit;
			/*  fatal("[ mii_access(): got opcode = %i ]\n",
			    d->mii_opcode);  */
			d->mii_state = MII_STATE_READ_PHYADDR_REGADDR;
		}
		d->mii_bit ++;
		break;

	case MII_STATE_READ_PHYADDR_REGADDR:
		/*  fatal("[ mii_access(): got phy/reg addr bit nr %i (%i)"
		    " ]\n", d->mii_bit - 2, obit);  */
		if (d->mii_bit <= 6)
			d->mii_phyaddr |= obit << (6-d->mii_bit);
		else
			d->mii_regaddr |= obit << (11-d->mii_bit);
		d->mii_bit ++;
		if (d->mii_bit >= 12) {
			/*  fatal("[ mii_access(): phyaddr=0x%x regaddr=0x"
			    "%x ]\n", d->mii_phyaddr, d->mii_regaddr);  */
			d->mii_state = MII_STATE_A;
		}
		break;

	case MII_STATE_A:
		switch (d->mii_opcode) {
		case MII_COMMAND_WRITE:
			if (d->mii_bit >= 13)
				d->mii_state = MII_STATE_D;
			break;
		case MII_COMMAND_READ:
			ibit = 0;
			d->mii_state = MII_STATE_D;
			break;
		default:debug("[ mii_access(): UNIMPLEMENTED MII opcode "
			    "%i (probably just a bug in GXemul's "
			    "MII data stream handling) ]\n", d->mii_opcode);
			d->mii_state = MII_STATE_RESET;
		}
		d->mii_bit ++;
		break;

	case MII_STATE_D:
		switch (d->mii_opcode) {
		case MII_COMMAND_WRITE:
			if (idata & MIIROM_MIIDIR)
				fatal("[ mii_access(): write: bad dir? ]\n");
			obit = obit? (0x8000 >> (d->mii_bit - 14)) : 0;
			tmp = d->mii_phy_reg[(d->mii_phyaddr << 5) +
			    d->mii_regaddr] | obit;
			if (d->mii_bit >= 29) {
				d->mii_state = MII_STATE_IDLE;
				debug("[ mii_access(): WRITE to phyaddr=0x%x "
				    "regaddr=0x%x: 0x%04x ]\n", d->mii_phyaddr,
				    d->mii_regaddr, tmp);
			}
			break;
		case MII_COMMAND_READ:
			if (!(idata & MIIROM_MIIDIR))
				break;
			tmp = d->mii_phy_reg[(d->mii_phyaddr << 5) +
			    d->mii_regaddr];
			if (d->mii_bit == 13)
				debug("[ mii_access(): READ phyaddr=0x%x "
				    "regaddr=0x%x: 0x%04x ]\n", d->mii_phyaddr,
				    d->mii_regaddr, tmp);
			ibit = tmp & (0x8000 >> (d->mii_bit - 13));
			if (d->mii_bit >= 28)
				d->mii_state = MII_STATE_IDLE;
			break;
		}
		d->mii_bit ++;
		break;

	case MII_STATE_IDLE:
		d->mii_bit ++;
		if (d->mii_bit >= 31)
			d->mii_state = MII_STATE_RESET;
		break;
	}

	d->reg[CSR_MIIROM / 8] &= ~MIIROM_MDI;
	if (ibit)
		d->reg[CSR_MIIROM / 8] |= MIIROM_MDI;
}


/*
 *  srom_access():
 *
 *  This function handles reads from the Ethernet Address ROM. This is not a
 *  100% correct implementation, as it was reverse-engineered from OpenBSD
 *  sources; it seems to work with OpenBSD, NetBSD, and Linux, though.
 *
 *  Each transfer (if I understood this correctly) is of the following format:
 *
 *	1xx yyyyyy zzzzzzzzzzzzzzzz
 *
 *  where 1xx    = operation (6 means a Read),
 *        yyyyyy = ROM address
 *        zz...z = data
 *
 *  y and z are _both_ read and written to at the same time; this enables the
 *  operating system to sense the number of bits in y (when reading, all y bits
 *  are 1 except the last one).
 */
static void srom_access(struct cpu *cpu, struct dec21143_data *d,
	uint32_t oldreg, uint32_t idata)
{
	int obit, ibit;

	/*  debug("CSR9 WRITE! 0x%08x\n", (int)idata);  */

	/*  New selection? Then reset internal state.  */
	if (idata & MIIROM_SR && !(oldreg & MIIROM_SR)) {
		d->srom_curbit = 0;
		d->srom_opcode = 0;
		d->srom_opcode_has_started = 0;
		d->srom_addr = 0;
	}

	/*  Only care about data during clock cycles:  */
	if (!(idata & MIIROM_SROMSK))
		return;

	obit = 0;
	ibit = idata & MIIROM_SROMDI? 1 : 0;
	/*  debug("CLOCK CYCLE! (bit %i): ", d->srom_curbit);  */

	/*
	 *  Linux sends more zeroes before starting the actual opcode, than
	 *  OpenBSD and NetBSD. Hopefully this is correct. (I'm just guessing
	 *  that all opcodes should start with a 1, perhaps that's not really
	 *  the case.)
	 */
	if (!ibit && !d->srom_opcode_has_started)
		return;

	if (d->srom_curbit < 3) {
		d->srom_opcode_has_started = 1;
		d->srom_opcode <<= 1;
		d->srom_opcode |= ibit;
		/*  debug("opcode input '%i'\n", ibit);  */
	} else {
		switch (d->srom_opcode) {
		case TULIP_SROM_OPC_READ:
			if (d->srom_curbit < ROM_WIDTH + 3) {
				obit = d->srom_curbit < ROM_WIDTH + 2;
				d->srom_addr <<= 1;
				d->srom_addr |= ibit;
			} else {
				uint16_t romword = d->srom[d->srom_addr*2]
				    + (d->srom[d->srom_addr*2+1] << 8);
				if (d->srom_curbit == ROM_WIDTH + 3)
					debug("[ dec21143: ROM read from offset"
					    " 0x%03x: 0x%04x ]\n",
					    d->srom_addr, romword);
				obit = romword & (0x8000 >>
				    (d->srom_curbit - ROM_WIDTH - 3))? 1 : 0;
			}
			break;
		default:fatal("[ dec21243: unimplemented SROM/EEPROM "
			    "opcode %i ]\n", d->srom_opcode);
		}
		d->reg[CSR_MIIROM / 8] &= ~MIIROM_SROMDO;
		if (obit)
			d->reg[CSR_MIIROM / 8] |= MIIROM_SROMDO;
		/*  debug("input '%i', output '%i'\n", ibit, obit);  */
	}

	d->srom_curbit ++;

	/*
	 *  Done opcode + addr + data? Then restart. (At least NetBSD does
	 *  sequential reads without turning selection off and then on.)
	 */
	if (d->srom_curbit >= 3 + ROM_WIDTH + 16) {
		d->srom_curbit = 0;
		d->srom_opcode = 0;
		d->srom_opcode_has_started = 0;
		d->srom_addr = 0;
	}
}


/*
 *  dec21143_reset():
 *
 *  Set the 21143 registers, SROM, and MII data to reasonable values.
 */
static void dec21143_reset(struct cpu *cpu, struct dec21143_data *d)
{

	if (d->cur_rx_buf != NULL)
		free(d->cur_rx_buf);
	if (d->cur_tx_buf != NULL)
		free(d->cur_tx_buf);
	d->cur_rx_buf = d->cur_tx_buf = NULL;

	memset(d->reg, 0, sizeof(uint32_t) * N_REGS);
	memset(d->mii_phy_reg, 0, sizeof(d->mii_phy_reg));

	/*  Register values at reset, according to the manual:  */
	d->reg[CSR_BUSMODE / 8] = 0xfe000000;	/*  csr0   */
	d->reg[CSR_MIIROM  / 8] = 0xfff483ff;	/*  csr9   */
	d->reg[CSR_SIACONN / 8] = 0xffff0000;	/*  csr13  */
	d->reg[CSR_SIATXRX / 8] = 0xffffffff;	/*  csr14  */
	d->reg[CSR_SIAGEN  / 8] = 0x8ff00000;	/*  csr15  */

	d->tx_idling_threshold = 10;
	d->cur_rx_addr = d->cur_tx_addr = 0;

	/*  Set the MAC address:  */
	memcpy(d->nic.mac_address, d->srom + TULIP_ROM_IEEE_NETWORK_ADDRESS, 6);

	/*  MII PHY initial state:  */
	d->mii_state = MII_STATE_RESET;

	/*  PHY #0:  */
	d->mii_phy_reg[MII_BMSR] = BMSR_100TXFDX | BMSR_10TFDX |
	    BMSR_ACOMP | BMSR_ANEG | BMSR_LINK;
}


DEVICE_ACCESS(dec21143)
{
	struct dec21143_data *d = (struct dec21143_data *) extra;
	uint32_t idata = 0, odata = 0;
	uint32_t oldreg = 0;
	int regnr = relative_addr >> 3;

	if (writeflag == MEM_WRITE)
		idata = (uint32_t)memory_readmax64(cpu, data,
		    len | d->pci_little_endian);

	if ((relative_addr & 7) == 0 && regnr < N_REGS) {
		if (writeflag == MEM_READ) {
			odata = d->reg[regnr];
		} else {
			oldreg = d->reg[regnr];
			switch (regnr) {
			case CSR_STATUS / 8:	/*  Zero-on-write  */
				d->reg[regnr] &= ~(idata & 0x0c01ffff);
				break;
			case CSR_MISSED / 8:	/*  Read only  */
				break;
			default:d->reg[regnr] = idata;
			}
		}
	} else
		fatal("[ dec21143: WARNING! unaligned access (0x%x) ]\n",
		    (int)relative_addr);

	switch (relative_addr) {

	case CSR_BUSMODE:	/*  csr0  */
		if (writeflag == MEM_WRITE) {
			/*  Software reset takes effect immediately.  */
			if (idata & BUSMODE_SWR) {
				dec21143_reset(cpu, d);
				idata &= ~BUSMODE_SWR;
			}
		}
		break;

	case CSR_TXPOLL:	/*  csr1  */
		if (writeflag == MEM_READ)
			fatal("[ dec21143: UNIMPLEMENTED READ from "
			    "txpoll ]\n");
		d->tx_idling = d->tx_idling_threshold;
		dev_dec21143_tick(cpu, extra);
		break;

	case CSR_RXPOLL:	/*  csr2  */
		if (writeflag == MEM_READ)
			fatal("[ dec21143: UNIMPLEMENTED READ from "
			    "rxpoll ]\n");
		dev_dec21143_tick(cpu, extra);
		break;

	case CSR_RXLIST:	/*  csr3  */
		if (writeflag == MEM_WRITE) {
			debug("[ dec21143: setting RXLIST to 0x%x ]\n",
			    (int)idata);
			if (idata & 0x3)
				fatal("[ dec21143: WARNING! RXLIST not aligned"
				    "? (0x%llx) ]\n", (long long)idata);
			idata &= ~0x3;
			d->cur_rx_addr = idata;
		}
		break;

	case CSR_TXLIST:	/*  csr4  */
		if (writeflag == MEM_WRITE) {
			debug("[ dec21143: setting TXLIST to 0x%x ]\n",
			    (int)idata);
			if (idata & 0x3)
				fatal("[ dec21143: WARNING! TXLIST not aligned"
				    "? (0x%llx) ]\n", (long long)idata);
			idata &= ~0x3;
			d->cur_tx_addr = idata;
		}
		break;

	case CSR_STATUS:	/*  csr5  */
	case CSR_INTEN:		/*  csr7  */
		if (writeflag == MEM_WRITE) {
			/*  Recalculate interrupt assertion.  */
			dev_dec21143_tick(cpu, extra);
		}
		break;

	case CSR_OPMODE:	/*  csr6:  */
		if (writeflag == MEM_WRITE) {
			if (idata & 0x02000000) {
				/*  A must-be-one bit.  */
				idata &= ~0x02000000;
			}
			if (idata & OPMODE_ST) {
				idata &= ~OPMODE_ST;
			} else {
				/*  Turned off TX? Then idle:  */
				d->reg[CSR_STATUS/8] |= STATUS_TPS;
			}
			if (idata & OPMODE_SR) {
				idata &= ~OPMODE_SR;
			} else {
				/*  Turned off RX? Then go to stopped state:  */
				d->reg[CSR_STATUS/8] &= ~STATUS_RS;
			}
			/*  Maintain r/o filter mode bits:  */
			d->reg[CSR_OPMODE/8] &=
			    ~(OPMODE_HP | OPMODE_HO | OPMODE_IF);
			d->reg[CSR_OPMODE/8] |= oldreg &
			    (OPMODE_HP | OPMODE_HO | OPMODE_IF);
			idata &= ~(OPMODE_HBD | OPMODE_SCR | OPMODE_PCS
			    | OPMODE_PS | OPMODE_SF | OPMODE_TTM | OPMODE_FD
			    | OPMODE_IF | OPMODE_HO | OPMODE_HP | OPMODE_PR
			    | OPMODE_PM);
			if (idata & OPMODE_PNIC_IT) {
				idata &= ~OPMODE_PNIC_IT;
				d->tx_idling = d->tx_idling_threshold;
			}
			if (idata != 0) {
				fatal("[ dec21143: UNIMPLEMENTED OPMODE bits"
				    ": 0x%08x ]\n", (int)idata);
			}
			dec21143_update_rx_mode(d);
			dev_dec21143_tick(cpu, extra);
		}
		break;

	case CSR_MISSED:	/*  csr8  */
		break;

	case CSR_MIIROM:	/*  csr9  */
		if (writeflag == MEM_WRITE) {
			if (idata & MIIROM_MDC)
				mii_access(cpu, d, oldreg, idata);
			else
				srom_access(cpu, d, oldreg, idata);
		}
		break;

	case CSR_SIASTAT:	/*  csr12  */
		/*  Auto-negotiation status = Good.  */
		odata = SIASTAT_ANS_FLPGOOD;
		break;

	case CSR_SIATXRX:	/*  csr14  */
		/*  Auto-negotiation Enabled  */
		odata = SIATXRX_ANE;
		break;

	case CSR_SIACONN:	/*  csr13  */
	case CSR_SIAGEN:	/*  csr15  */
		/*  Don't print warnings for these, for now.  */
		break;

	default:if (writeflag == MEM_READ)
			fatal("[ dec21143: read from unimplemented 0x%02x ]\n",
			    (int)relative_addr);
		else
			fatal("[ dec21143: write to unimplemented 0x%02x: "
			    "0x%02x ]\n", (int)relative_addr, (int)idata);
	}

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len | d->pci_little_endian, odata);

	return 1;
}


DEVINIT(dec21143)
{
	struct dec21143_data *d;
	char name2[100];
	int leaf;

	CHECK_ALLOCATION(d = (struct dec21143_data *) malloc(sizeof(struct dec21143_data)));
	memset(d, 0, sizeof(struct dec21143_data));

	INTERRUPT_CONNECT(devinit->interrupt_path, d->irq);
	d->pci_little_endian = devinit->pci_little_endian;

	/* XXX XXX XXX */
	switch (devinit->machine->machine_type) {
	/*
	 * Footbridge systems -- this is actually configurable by
	 * system software, but this is the window setting that
	 * NetBSD uses.
	 */
	case MACHINE_CATS:
	case MACHINE_NETWINDER:
		d->xxx_dma_to_phys_mask = ~0x20000000;
		break;

	/*
	 * V3 Semi PCI bus controller -- this is actually configurable
	 * by system sofware, but this is the value previously hard-coded
	 * for all platforms that did not work on Footbridge systems.
	 */
	case MACHINE_ALGOR:
		/* FALLTHROUGH */

	/* Other known users of dc21143 that came along for the ride. */
	case MACHINE_COBALT:
	case MACHINE_PMPPC:
	case MACHINE_PREP:
	case MACHINE_MACPPC:
	case MACHINE_MVMEPPC:
		d->xxx_dma_to_phys_mask = 0x7fffffff;
		break;

	default:
		fatal("[ dec21143: default DMA mask for unhandled machine %d\n",
		      devinit->machine->machine_type);
		d->xxx_dma_to_phys_mask = 0xffffffff;
	}

	memset(d->srom, 0, sizeof(d->srom));

	/*  Version (= 1) and Chip count (= 1):  */
	d->srom[TULIP_ROM_SROM_FORMAT_VERION] = 1;
	d->srom[TULIP_ROM_CHIP_COUNT] = 1;

	leaf = 30;
	d->srom[TULIP_ROM_CHIPn_DEVICE_NUMBER(0)] = 0;
	d->srom[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0)] = leaf & 255;
	d->srom[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0)+1] = leaf >> 8;

	d->srom[leaf+TULIP_ROM_IL_SELECT_CONN_TYPE] = 0; /*  Not used?  */
	d->srom[leaf+TULIP_ROM_IL_MEDIA_COUNT] = 2;
	leaf += TULIP_ROM_IL_MEDIAn_BLOCK_BASE;

	d->srom[leaf] = 7;	/*  descriptor length  */
	d->srom[leaf+1] = TULIP_ROM_MB_21142_SIA;
	d->srom[leaf+2] = TULIP_ROM_MB_MEDIA_100TX;
	/*  here comes 4 bytes of GPIO control/data settings  */
	leaf += d->srom[leaf];

	d->srom[leaf] = 15;	/*  descriptor length  */
	d->srom[leaf+1] = TULIP_ROM_MB_21142_MII;
	d->srom[leaf+2] = 0;	/*  PHY nr  */
	d->srom[leaf+3] = 0;	/*  len of select sequence  */
	d->srom[leaf+4] = 0;	/*  len of reset sequence  */
	/*  5,6, 7,8, 9,10, 11,12, 13,14 = unused by GXemul  */
	leaf += d->srom[leaf];

	net_generate_unique_mac(devinit->machine, d->nic.mac_address);
	memcpy(d->srom + TULIP_ROM_IEEE_NETWORK_ADDRESS, d->nic.mac_address, 6);
	net_add_nic(devinit->machine->emul->net, &d->nic);

	dec21143_reset(devinit->machine->cpus[0], d);

	snprintf(name2, sizeof(name2), "%s [%02x:%02x:%02x:%02x:%02x:%02x]",
	    devinit->name, d->nic.mac_address[0], d->nic.mac_address[1],
	    d->nic.mac_address[2], d->nic.mac_address[3],
	    d->nic.mac_address[4], d->nic.mac_address[5]);

	memory_device_register(devinit->machine->memory, name2,
	    devinit->addr, 0x100, dev_dec21143_access, d, DM_DEFAULT, NULL);

	machine_add_tickfunction(devinit->machine,
	    dev_dec21143_tick, d, DEC21143_TICK_SHIFT);

	/*
	 *  NetBSD/cats uses memory accesses, OpenBSD/cats uses I/O registers.
	 *  Let's make a mirror from the memory range to the I/O range:
	 */
	dev_ram_init(devinit->machine, devinit->addr2, 0x100, DEV_RAM_MIRROR
	    | DEV_RAM_MIGHT_POINT_TO_DEVICES, devinit->addr, NULL);

	return 1;
}

