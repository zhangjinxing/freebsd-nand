/*
 * Copyright (C) 2009 Andrew Turner
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>

#include <dev/nand/nandvar.h>

#include <arm/s3c2xx0/s3c2410reg.h>
#include <arm/s3c2xx0/s3c2440reg.h>
#include <arm/s3c2xx0/s3c2410var.h>

struct s3c24x0_nand_softc {
	struct s3c2xx0_softc	 sc_sx;

	struct nand_device	 sc_nand_dev;

	bus_space_handle_t	 sc_nand_ioh;

	bus_size_t		 sc_cmd_reg;
	bus_size_t		 sc_addr_reg;
	bus_size_t		 sc_data_reg;
	bus_size_t		 sc_stat_reg;
	bus_size_t		 sc_ce_reg;
	uint32_t		 sc_ce_mask;
};

static int	s3c24x0_nand_probe(device_t);
static int	s3c24x0_nand_attach(device_t);

static int	s3c24x0_select(nand_device_t, int);
static int	s3c24x0_nand_command(nand_device_t, uint8_t);
static int	s3c24x0_nand_address(nand_device_t, uint8_t);
static int	s3c24x0_nand_read(nand_device_t, size_t, uint8_t *);
static int	s3c24x0_nand_read_8(nand_device_t, uint8_t *);
static int	s3c24x0_nand_write(nand_device_t, size_t, uint8_t *);
static int	s3c24x0_read_rnb(nand_device_t);
static int	s3c24x0_init_ecc(nand_device_t);
static int	s3c24x0_calc_ecc(nand_device_t, uint8_t *);
static int	s3c24x0_fix_data(nand_device_t, size_t, uint8_t *, uint8_t *,
    uint8_t *);

static struct nand_driver s3c24x0_nand_dri = {
	.ndri_select = s3c24x0_select,
	.ndri_command = s3c24x0_nand_command,
	.ndri_address = s3c24x0_nand_address,
	.ndri_read = s3c24x0_nand_read,
	.ndri_read_8 = s3c24x0_nand_read_8,
	.ndri_write = s3c24x0_nand_write,
	.ndri_read_rnb = s3c24x0_read_rnb,
	.ndri_init_ecc = s3c24x0_init_ecc,
	.ndri_calc_ecc = s3c24x0_calc_ecc,
	.ndri_fix_data = s3c24x0_fix_data,
};

struct nand_ecc_data s3c2410_nand_ecc = {
	.ecc_size = 3,
	.ecc_stride = 3,
	.ecc_protect = 512,
	.ecc_pos = { 0, 1, 2 },
};

static void
s3c24x0_nand_init(struct s3c24x0_nand_softc *sc)
{
	uint32_t reg;
	bus_space_handle_t ioh;
	bus_space_tag_t iot;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	/* 
	 * Ensure the NAND is enabled and on
	 */
	switch (s3c2xx0_softc->sc_cpu) {
	case CPU_S3C2440:
		reg = S3C2440_NFCONT_ENABLE;
		bus_space_write_4(iot, ioh, S3C2440_NANDFC_NFCONT, reg);

		sc->sc_cmd_reg = S3C2440_NANDFC_NFCMMD;
		sc->sc_addr_reg = S3C2440_NANDFC_NFADDR;
		sc->sc_data_reg = S3C2440_NANDFC_NFDATA;
		sc->sc_stat_reg = S3C2440_NANDFC_NFSTAT;
		sc->sc_ce_reg = S3C2440_NANDFC_NFCONT;
		sc->sc_ce_mask = S3C2440_NFCONT_NCE;
		break;
	case CPU_S3C2410:
		reg = bus_space_read_4(iot, ioh, NANDFC_NFCONF);
		reg |= S3C2410_NFCONF_ENABLE;
		bus_space_write_4(iot, ioh, NANDFC_NFCONF, reg);

		sc->sc_cmd_reg = S3C2410_NANDFC_NFCMD;
		sc->sc_addr_reg = S3C2410_NANDFC_NFADDR;
		sc->sc_data_reg = S3C2410_NANDFC_NFDATA;
		sc->sc_stat_reg = S3C2410_NANDFC_NFSTAT;
		sc->sc_ce_reg = NANDFC_NFCONF;
		sc->sc_ce_mask = S3C2410_NFCONF_FCE;
		break;
	default:
		panic("Unknown processor");
	}

}

static int
s3c24x0_nand_probe(device_t dev)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(dev);
	bus_size_t size;
	int ret;

	switch (s3c2xx0_softc->sc_cpu) {
	case CPU_S3C2410:
		size = S3C2410_NANDFC_SIZE;
		break;
	case CPU_S3C2440:
		size = S3C2440_NANDFC_SIZE;
		break;
	default:
		return (ENXIO);
	}

	sc->sc_sx.sc_iot = &s3c2xx0_bs_tag;
	if (bus_space_map(sc->sc_sx.sc_iot, S3C24X0_NANDFC_BASE, size, 0,
	    &sc->sc_nand_ioh))
		panic("Cannot map NAND registers");

	/* Init the NAND Controller enough to talk to the device */
	s3c24x0_nand_init(sc);
	sc->sc_nand_dev.ndev_driver = &s3c24x0_nand_dri;
	sc->sc_nand_dev.ndev_dev = dev;

	ret = nand_probe(&sc->sc_nand_dev);

	bus_space_unmap(sc->sc_sx.sc_iot, sc->sc_nand_ioh, S3C2410_NANDFC_SIZE);

	return ret;
}

static int
s3c24x0_nand_attach(device_t dev)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(dev);
	int err;

	if (bus_space_map(sc->sc_sx.sc_iot, S3C24X0_NANDFC_BASE,
	    S3C2410_NANDFC_SIZE * 2, 0, &sc->sc_nand_ioh))
		panic("Cannot map NAND registers");

	/* Make sure the Flash is in a consistent state before use */
	s3c24x0_nand_init(sc);

	sc->sc_nand_dev.ndev_ecc = &s3c2410_nand_ecc;

	err = nand_attach(&sc->sc_nand_dev);
	if (err != 0) {
		device_set_desc(dev, sc->sc_nand_dev.ndev_name);
	}

	return (err);
}

static int
s3c24x0_select(nand_device_t ndev, int enable)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	uint32_t reg;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	reg = bus_space_read_4(iot, ioh, sc->sc_ce_reg);
	if (enable)
		reg &= ~(sc->sc_ce_mask);
	else
		reg |= sc->sc_ce_mask;
	bus_space_write_4(iot, ioh, sc->sc_ce_reg, reg);

	return (0);
}

static int
s3c24x0_nand_command(nand_device_t ndev, uint8_t cmd)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	bus_space_write_1(iot, ioh, sc->sc_cmd_reg, cmd);

	return (0);
}

static int
s3c24x0_nand_address(nand_device_t ndev, uint8_t addr)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	bus_space_write_1(iot, ioh, sc->sc_addr_reg, addr);

	return (0);
}

static int
s3c24x0_nand_read(nand_device_t ndev, size_t len, uint8_t *data)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	size_t pos;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	for (pos = 0; pos < len; pos++) {
		data[pos] = bus_space_read_4(iot, ioh, sc->sc_data_reg) & 0xFF;
	}

	return (0);
}

static int
s3c24x0_nand_read_8(nand_device_t ndev, uint8_t *data)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	*data = bus_space_read_1(iot, ioh, sc->sc_data_reg);

	return (0);
}

static int
s3c24x0_nand_write(nand_device_t ndev, size_t len, uint8_t *data)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	size_t pos;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	for (pos = 0; pos < len; pos++) {
		bus_space_write_1(iot, ioh, sc->sc_data_reg, data[pos]);
	}

	return (0);
}

static int
s3c24x0_read_rnb(nand_device_t ndev)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	int rnb;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	rnb = bus_space_read_1(iot, ioh, sc->sc_stat_reg) & NFSTAT_READY;
	return (rnb == NFSTAT_READY);
}

static int
s3c24x0_init_ecc(nand_device_t ndev)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	uint32_t nfconf;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	nfconf = bus_space_read_4(iot, ioh, NANDFC_NFCONF);
	nfconf |= S3C2410_NFCONF_ECC;
	bus_space_write_4(iot, ioh, NANDFC_NFCONF, nfconf);

	return (0);
}

static int
s3c24x0_calc_ecc(nand_device_t ndev, uint8_t *ecc)
{
	struct s3c24x0_nand_softc *sc = device_get_softc(ndev->ndev_dev);
	bus_space_handle_t ioh;
	bus_space_tag_t iot;
	uint32_t ecctmp;

	iot = sc->sc_sx.sc_iot;
	ioh = sc->sc_nand_ioh;

	ecctmp = bus_space_read_4(iot, ioh, S3C2410_NANDFC_NFECC);
	ecc[0] = ecctmp & 0xFF;
	ecc[1] = (ecctmp >> 8) & 0xFF;
	ecc[2] = (ecctmp >> 16) & 0xFF;

	return (0);
}

static int
s3c24x0_fix_data(nand_device_t ndev, size_t len, uint8_t *data,
    uint8_t *calc_ecc, uint8_t *read_ecc)
{
	uint8_t diff[3];

	diff[0] = calc_ecc[0] ^ read_ecc[0];
	diff[1] = calc_ecc[1] ^ read_ecc[1];
	diff[2] = calc_ecc[2] ^ read_ecc[2];

	/* The two ECC's are the same so are correct */
	if (diff[0] == 0x00 && diff[1] == 0x00 && diff[2] == 0x00)
		return (0);
	/* There may be no ECC, ignore this case */
	if (read_ecc[0] == 0xFF && read_ecc[1] == 0xFF && read_ecc[2] == 0xFF)
		return (0);

	/* TODO: Fix the data if we are can */
	device_printf(ndev->ndev_dev, "Bad ECC: %X %X %X != %X %X %X\n",
	    calc_ecc[0], calc_ecc[1], calc_ecc[2],
	    read_ecc[0], read_ecc[1], read_ecc[2]);
	return (EIO);
}

static device_method_t s3c2410_nand_methods[] = {
	DEVMETHOD(device_probe, s3c24x0_nand_probe),
	DEVMETHOD(device_attach, s3c24x0_nand_attach),

	{0, 0},
};

static driver_t nand_s3c2410_driver = {
	"s3c24x0_nand",
	s3c2410_nand_methods,
	sizeof(struct s3c24x0_nand_softc),
};

static devclass_t nand_devclass;

DRIVER_MODULE(s3c24x0_nand, s3c24x0, nand_s3c2410_driver, nand_devclass, 0, 0);
MODULE_DEPEND(s3c24x0_nand, nand, 1, 1, 1);

