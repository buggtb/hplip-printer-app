# HPLIP Printer Application

## INTRODUCTION

This repository contains a Printer Application for printing on HP and
Apollo printers supported by HP's HPLIP driver suite.

It uses [PAPPL](https://www.msweet.org/pappl) to support IPP
printing from multiple operating systems. In addition, it uses the
resources of [cups-filters
2.x](https://github.com/OpenPrinting/cups-filters) (filter functions
in libcupsfilters, libppd) and
[pappl-retrofit](https://github.com/OpenPrinting/pappl-retrofit)
(encapsulating classic CUPS drivers in Printer Applications). This
work (or now the code of pappl-retrofit) is derived from the
[hp-printer-app](https://github.com/michaelrsweet/hp-printer-app).

The printer driver itself and the software to communicate with the
printer hardware is taken from the [HPLIP (HP Linux Imaging and
Printing) driver
suite](https://developers.hp.com/hp-linux-imaging-and-printing/), also
the Information about supported printer models and their capabilities.

Your contributions are welcome. Please post [issues and pull
requests](https://github.com/OpenPrinting/hplip-printer-app).

**Note: HPLIP is actively maintained by HP, they are continuously
adding the newest printer models and adapting the software to new
environments/Linux distributions. Therefore it would also be the
correct way if HP turns HPLIP into a Printer Application or at least
offers this as an alternative to the classic CUPS/SANE
driver. Especially they should create a native Printer Application,
meaning that it does not use PPDs, CUPS filters, and CUPS backends
internally. Also their utilities need to be made independent of
CUPS.**

For PostScript printers you can also use the [PostScript Printer
Application](https://github.com/OpenPrinting/ps-printer-app),
especially if you have it already installed for some non-HP PostScript
printer.

Also check whether your printer is a driverless IPP printer (AirPrint,
Mopria, IPP Everywhere, Wi-Fi Direct Print, prints from phones) as in
this case you do not need any Printer Application at all. Most modern
HP printers, even the cheapest models, are driverless IPP
printers. Even USB-only printers can be driverless IPP, and you can
generally use driverless IPP via USB, try
[ipp-usb](https://github.com/OpenPrinting/ipp-usb) for these cases
first.


## Project Bluefin FSDK OCI appliance

Upstream `snap/` and `rockcraft.yaml` remain as source references, but this
fork builds and releases only the BuildStream OCI image.
It builds the Debian-patched HPLIP print drivers (`hpcups`, `hpps`, `hp`, `HP`,
`hp-probe` and their PPDs) on the shared printing base
(`fsdk-containers.bst:printing/base.bst`) from the pinned fsdk-containers
junction (`elements/fsdk-containers.bst`). The base is the one patched CUPS,
cups-filters, Ghostscript, Avahi and PAPPL stack for all printer
applications, and FSDK is reached only through it; the image does not start
`cupsd`. The final image composes runtime domains only. This appliance is
**print-only**. A socket-sink print job is not evidence of scanning, firmware
upload to real hardware or paper output.

The BuildStream sandbox has no passwd record for its build UID, so HPLIP's
`dat2drv.py` uses the explicitly supplied build `USER` and `HOME` when NSS
has no entry. Its network-discovery library links against the base's
nonroot Avahi (`avahi-printing`), while documentation installs into the
source's valid default path and is excluded from the runtime composition.

On native x86_64 or aarch64, with Podman, FUSE and `just` available:

```sh
just validate
just fetch
just verify
```

`just verify` builds the actual OCI image, starts it as numeric user 65532,
prints through PAPPL, `hpcups` and the CUPS socket backend into a TCP sink,
then checks the PCL raster output and persisted state. It also checks HTTPS,
supervised D-Bus/Avahi shutdown, required-child failure, and that the image
carries no devel content (`just check-no-devel`). The service uses port 18030
by default; assign a different unprivileged `PORT` for each simultaneously
running printer family.

For a rootless deployment, dedicate a volume to **this** application:

```sh
mkdir -p hplip-state
podman unshare chown 65532:65532 hplip-state
podman run --name hplip-printer-app --network host \
  --hostname hplip-printer-app -e PORT=18030 \
  -v "$PWD/hplip-state:/var/lib/hplip-printer-app:Z" \
  ghcr.io/projectbluefin/hplip-printer-app:3.26.4
```

Persist the same volume across restarts. The web UI is available at
`https://localhost:18030/`; HP's proprietary plugin is opt-in under `/plugin`.
The rootless application downloads it only after web consent, checks HP's
published size, checksum and GPG signature using the public key from the
Debian source, and installs it into the volume. Neither the plugin nor its
firmware is part of the image. Keep the volume private to its numeric owner.

An empty volume starts with the optional plugin not installed; an unreadable
status file still produces an error log.

Network/USB discovery requires permissions and a reachable network; if an HP
USB printer is assigned, pass **only** its `/dev/bus/usb` device with Podman's
`--device` and grant the mapped user device access (for example via host udev
group ownership plus `--group-add keep-groups`). Never assign the same device
to another printer family's appliance. With `--network host`, give each
appliance a different port and hostname, and give printers on different
appliances distinct names, so DNS-SD advertisements do not collide.
Physical device discovery, plugin firmware load and paper output remain
unverified without supported hardware.

### Authenticating or disabling the web admin interface

Because this appliance runs with `--network host`, the web admin UI is
LAN-reachable and the only boundary is authorization. By default PAPPL
leaves it open to anyone who can reach the port. Set one of the following
before starting the container; the entrypoint validates every value and
stops instead of ever falling back to an unauthenticated admin UI (exit `64`
for a malformed value, `78` for a value this image cannot honour):

- `PRINTER_APP_AUTH_SERVICE=<name>` — reserved for authenticating web admin
  requests (`-o auth-service=<name>`) once PAM is available. The shared
  printing base builds PAPPL without PAM, so any service name would lock every
  administrator out, including from `localhost`; the entrypoint refuses every
  well-formed value with exit `78`, whether or not `/etc/pam.d/<name>` exists
  (`/etc/pam.d/cups` ships with CUPS).
- `PRINTER_APP_ADMIN_GROUP=<group>` — the OS group allowed to administer the
  server (`-o admin-group=<group>`). Requires `PRINTER_APP_AUTH_SERVICE`, so
  it is refused as well (exit `78`) until PAM is available.
- `PRINTER_APP_SERVER_OPTIONS=<opt[,opt...]>` — PAPPL server options forwarded
  as `-o server-options=<opts>`. Only `no-web-interface` is accepted; other
  PAPPL options either weaken the appliance (`no-tls`, `none`) or are already
  the default, so they are rejected with exit `64`. `no-web-interface`
  disables the web admin UI entirely while IPP printing keeps working.

`PRINTER_APP_SERVER_OPTIONS=no-web-interface` is therefore the only supported
way to secure the web admin interface today. It also removes the per-printer
"Device Settings" page that pappl-retrofit registers.

### Isolating LAN discovery and USB access between coexisting appliances

When more than one printer-family appliance (for example this HPLIP image
alongside a separate Ghostscript or PostScript printer-app) runs on the same
host, each container must be its own DNS-SD and USB owner:

- **Persistent state**: bind-mount a *dedicated* host volume per appliance to
  `/var/lib/hplip-printer-app` (`podman unshare chown 65532:65532 <dir>`
  first). Settings, the CUPS `snmp.conf`, USB quirk rules and any added
  printers live only in that volume and survive container restarts without
  being overwritten, because the entrypoint only seeds defaults for files
  that do not already exist in the volume.
- **Port**: set a distinct `PORT` (1024-65535) per appliance via `-e
  PORT=<port>`; the entrypoint rejects a non-numeric or out-of-range value
  before starting any service (exit code 64).
- **DNS-SD advertisement**: set a distinct `--hostname` per appliance.
  `avahi-daemon` runs with no configured `host-name` override, so it
  advertises under the container's own hostname, and two appliances given
  different hostnames never publish colliding `<host>.local` records, even
  when both use `--network host`. The DNS-SD *instance* name for an IPP
  printer service, however, is the printer's own name, not the container
  hostname; give printers on different appliances distinct names (as the
  `hplip-printer-app ... add` invocations in your deployment should already
  do) so their `_ipp._tcp` service records do not collide too.
- **USB device ownership**: pass only the specific printer's node under
  `/dev/bus/usb` to the one appliance that owns that physical device (for
  example `--device /dev/bus/usb/<bus>/<device>` or a narrowly scoped
  `--device-cgroup-rule`), and grant the container's numeric user (65532)
  access via host udev group ownership plus `--group-add keep-groups`.
  Never mount the same USB device, or the whole `/dev/bus/usb` tree
  read-write, into more than one printer family's appliance, since either
  could then claim and reset a device the other is using.
- `tests/coexistence.sh` (run via `just verify`) starts two instances of
  this image with distinct ports, hostnames and state volumes and checks
  that they answer independently, advertise under different hostnames, and
  keep configured printers and state files private to their own volume.
  This proves the two synthetic appliances do not collide with each other;
  it does not exercise real USB hardware, so shared-bus contention with an
  actual physical HP device remains unverified until hardware is available.
- The image ships none of Avahi's sample remote-login records: this
  appliance serves neither SSH nor SFTP, so advertising `_ssh._tcp` or
  `_sftp-ssh._tcp` under the host's name would misdirect LAN users.
  `just check-no-remote-login-records` (run via `just verify`) asserts the
  built image carries no `/etc/avahi/services/{ssh,sftp-ssh}.service`.
  `just verify-service-advertisements` is the host-network counterpart: on a
  quiet test LAN it browses real records before and after starting and
  restarting two instances, proving neither adds a remote-login record while
  each instance's own IPP queue still resolves on its distinct port. It
  needs host Avahi and `avahi-browse`, so it is not part of `just verify`,
  and it claims nothing about physical discovery or printed paper.

CI (`fsdk-ci.yml`) runs only `just validate` (a BuildStream graph check) on
pull requests. The merge queue and `workflow_dispatch` run the full native
x86_64 and aarch64 image build plus `just verify`.
Full builds restore BuildStream's local CAS, artifacts and source protos from
the GitHub Actions cache, one entry per arch. If the printing base is not
cached, they first seed it from fsdk-containers' cosign-verified
`ghcr.io/projectbluefin/printing-base-devel:<arch>-<key>` bundle; any failure
falls back to a local build. CI passes `ci/buildstream.conf` to every `bst`
call (`BST_FLAGS`), which fetches sources only from the Bluefin source cache.
`bst-cache.yml` refills the cache on pushes to `testing`, nightly and on
dispatch (saved only when an arch fits in 9000 MB uncompressed; a larger
cache fails the refill). Reset it with
`gh cache delete --all`. `update-base.yml` proposes fsdk-containers junction
bumps to `testing` daily.

PRs target `testing`; after a verified commit is promoted to `stable`, only
the matching `v<VERSION>` tag can publish an immutable amd64+arm64 GHCR index
with a signed SPDX SBOM and provenance. There are no mutable OCI `latest`,
`edge` or `stable` aliases. The org Renovate runner updates the HPLIP source
tag, application version and local Net-SNMP pin on `testing`; no inherited
Snap/Rockcraft workflow can update `stable`. Failed image checks block release.

The release gate derives FSDK metadata from the fsdk-containers commit pinned
in `elements/fsdk-containers.bst` and rejects mismatched image labels.
The release workflow pushes, signs (index and both architecture manifests),
attests and verifies everything by digest. It creates the `<VERSION>`,
`<VERSION>-x86_64` and `<VERSION>-aarch64` tags only after every check passes,
so a failed release leaves no tagged, unsigned image.

### Properties

- A Printer Application providing the `hpcups` printer driver and all
  printer's PPDs of HPLIP, supporting printing on most printers from
  HP and Apollo. This allows easy printing in high quality, including
  photos on photo paper. The `hpps` CUPS filter for PIN-protected
  printing on PostScript printers is also included.

- The printers are discovered with HPLIP, too. For USB printers the
  `hp` CUPS backend is used and for network printers the `hp-probe`
  utility (encapsulated in a script to behave as a CUPS backend).

- The communication with the printers is done by the `hp` CUPS backend
  and so (at least in case of USB) the IEEE-1284.4 packet protocol
  (protocol 7/1/3 on USB) is used and not a simple stream protocol
  (like the standard CUPS and PAPPL backends use). This way one should
  be able to print and scan simultaneously, or at least check printer
  status while printing. Not all printers support this protocol, if
  not, a standard streaming protocol is used. Also any other special
  functionality which requires the `hp` backend is supported. On the
  "Add Printer" web interface page under "Devices" select the "HPLIP
  (HP)" entries.

- Note that the `hp` backend does not allow bi-directional access to
  the printer. If you have a PostScript printer and prefer support for
  remote querying of the printer's accessory configuration instead of
  simultaneous printing and scanning, CUPS' standard backends for USB
  and network printers are also available.

- If you have an unusal system configuration or a personal firewall
  HP's backends will perhaps not discover your printer. Also in this
  situation the standard backends, including the fully manual "Network
  Printer" entry in combination with the hostname/IP field can be
  helpful.

- Use of CUPS' instead of PAPPL's standard backends makes quirk
  workarounds for USB printers with compatibility problems being used
  (and are editable) and the output can get sent to the printer via
  IPP, IPPS (encrypted!), and LPD in addition to socket (usually port
  9100). The SNMP backend can get configured (community, address
  scope).

- PWG Raster, Apple Raster or image input data to be printed on a
  non-PostScript printer does not get converted to PostScript or PDF,
  it is only converted/scaled to the required color space and
  resolution and then fed into the `hpcups` driver.

- For printing on non-PostScript printers PDF and PostScript input
  data is rendered into raster data using Ghostscript. Ghostscript is
  also used to convert PDF into PostScript for PostScript printers.

- The information about which printer models are supported and which
  are their capabilities is based on the PPD files included in
  HPLIP. They are packaged in the Snap as a compressed archive.

- Standard job IPP attributes are mapped to the driver's option
  settings best fitting to them so that users can print from any type
  of client (like for example a phone or IoT device) which only
  supports standard IPP attributes and cannot retrive the PPD
  options. Trays, media sizes, media types, and duplex can get mapped
  easily, but when it comes to color and quality it gets more complex,
  as relevant options differ a lot in the PPD files. Here we use an
  algorithm which automatically (who wants hand-edit ~3000 PPD files
  for the assignments) finds the right set of option settings for each
  combination of `print-color-mode` (`color`/`monochrome`),
  `print-quality` (`draft`/`normal`/`high`), and
  `print-content-optimize`
  (`auto`/`photo`/`graphics`/`text`/`text-and-graphics`) in the PPD of
  the current printer. So you have easy access to the full quality or
  speed of your printer without needing to deal with printer-specific
  option settings (the original options are still accessible via web
  admin interface).

- The Snap of the HPLIP Printer Application takes HPLIP's source code
  from Debian's packaging repository instead of directly from HP, as
  Debian's package has ~80 patches fixing bugs which are reported to
  HP but the patch not adopted upstream. So with the Snap users should
  get the same experience in reliability and quality as with the
  Debian package.

- Support for downloading the proprietary plugin of HPLIP via an
  additional page in the web interface. This adds support for some
  laser printers which need their firmware loaded every time they are
  turned on or which use certain proprietary print data formats. This
  works both in the Snap and in the classic installation of the
  Printer Application (must run as root, otherwise only status check
  of the plugin).

  Installing and removing the plugin exchanges the plugin directory and
  registers the installed version in the state file. Both are done as a
  durable transaction, so that the Printer Application getting restarted
  or killed in the middle of it, especially when it is a container which
  is replaced during an update, does not lose the plugin which is
  currently installed, does not leave a plugin directory and a
  registered version which do not belong together, and does not leave a
  leftover copy of the downloaded plugin behind. An installation which
  was interrupted after the downloaded plugin was already verified and
  put in place is completed on the next start, so the plugin does not
  need to be downloaded again. The web interface always reports the
  version of the plugin which is actually installed.

### To Do

- Support for scanning on HP's multi-function printers. this requires
  scanning support in PAPPL (which made [good progress in GSoC
  2021](https://github.com/michaelrsweet/pappl/commits/scanning)).

- PDF test page, for example generated with the bannertopdf filter.

- Human-readable strings for vendor options (Needs support by PAPPL:
  [Issue #58: Localization
  support](https://github.com/michaelrsweet/pappl/issues/58))

- Internationalization/Localization (Needs support by PAPPL: [Issue
  #58: Localization
  support](https://github.com/michaelrsweet/pappl/issues/58))

- SNMP Ink level check via ps_status() function (Needs support by PAPPL:
  [Issue #83: CUPS does IPP and SNMP ink level polls via backends,
  PAPPL should have functions for
  this](https://github.com/michaelrsweet/pappl/issues/83))

- Build options for cups-filters, to build without libqpdf and/or
  without libppd, the former will allow to create the Snap of this
  Printer Application without downloading and building QPDF


## THE SNAP

### Installing and building

To just run and use this Printer Application, simply install it from
the Snap Store:

```
sudo snap install --edge hplip-printer-app
```

Then follow the instructions below for setting it up.

To build the Snap by yourself, in the main directory of this
repository run

```
snapcraft snap
```

This will download all needed packages and build the HPLIP Printer
Application. Note that PAPPL (upcoming 1.0) and cups-filters (upcoming
2.0) are pulled directly from their GIT repositories, as there are no
appropriate releases yet. This can also lead to the fact that this
Printer Application will suddenly not build any more.

To install the resulting Snap run

```
sudo snap install --dangerous hplip-printer-app_1.0_amd64.snap
```


### Setting up

The Printer Application will automatically be started as a server daemon.

Enter the web interface

```
http://localhost:8000/
```

Use the web interface to add a printer. Supply a name, select the
discovered printer, then select make and model. Also set the installed
accessories, loaded media and the option defaults. If the printer is a
PostScript printer, accessory configuration and option defaults can
also often get polled from the printer.

If the entry of your printer in the web interface has the remark
"requires proprietary plugin", you need to install HP's plugin. For
this, click on the "Plugin" button in this printer entry or on the
"Install Proprietary Plugin" button under "Other Settings" on the
front page of the web interface and follow the instructions on the
screen.

**Architecture note:** HP's vendor plugin archive bundles separate
`.so` files per CPU architecture (tagged internally as `x86_64`,
`x86_32`, `arm32`, or `arm64`), and only the file matching the
architecture of the running OCI image (`amd64`/`x86_64` or
`arm64`/`aarch64`) gets linked in. Not every vendor plugin release has
historically shipped `arm64` builds for every printer-support
component, so some HP models that require the proprietary plugin may
work on `amd64` but not yet be usable through the plugin path on
`arm64` if HP's archive lacks an `arm64` build for the needed
component. If no library file matches the running architecture, plugin
installation now fails explicitly instead of silently reporting
success.

Then print PDF, PostScript, JPEG, Apple Raster, or PWG Raster files
with

```
hplip-printer-app FILE
```

or print with CUPS, CUPS (and also cups-browsed) discover and treat
the printers set up with this Printer Application as driverless IPP
printers (IPP Everywhere and AirPrint).

See

```
hplip-printer-app --help
```

for more options.

Use the "-o log-level=debug" argument for verbose logging in your
terminal window.

You can add files to `/var/snap/hplip-printer-app/common/usb/` for
additional USB quirk rules. Edit the existing files only for quick
tests, as they get replaced at every update of the Snap (to introduce
new rules).

You can edit the `/var/snap/hplip-printer-app/common/cups/snmp.conf`
file for configuring SNMP network printer discovery.

## THE ROCK (OCI CONTAINER IMAGE)

### Install from Docker Hub
#### Prerequisites

1. **Docker Installed**: Ensure Docker is installed on your system. You can download it from the [official Docker website](https://www.docker.com/get-started).

#### Step-by-Step Guide

You can pull the `hplip-printer-app` Docker image from either the GitHub Container Registry or Docker Hub.

**From GitHub Container Registry** <br>
To pull the image from the GitHub Container Registry, run the following command:
```sh
  sudo docker pull ghcr.io/openprinting/hplip-printer-app:latest
```

Create a Docker volume:
```sh
  sudo docker volume create hplip-printer-app
```

To run the container after pulling the image from the GitHub Container Registry, use:
```sh
  sudo docker run -d \
      --name hplip-printer-app \
      --network host \
      -e PORT=<port> \
      -v hplip-printer-app:/var/lib/hplip-printer-app \
      -v /dev/bus/usb:/dev/bus/usb:ro \
      --device-cgroup-rule='c 189:* rmw' \
      ghcr.io/openprinting/hplip-printer-app:latest
```

**From Docker Hub** <br>
Alternatively, you can pull the image from Docker Hub, by running:
```sh
  sudo docker pull openprinting/hplip-printer-app
```

Create a Docker volume:
```sh
  sudo docker volume create hplip-printer-app
```

To run the container after pulling the image from Docker Hub, use:
```sh
  sudo docker run -d \
      --name hplip-printer-app \
      --network host \
      -e PORT=<port> \
      -v hplip-printer-app:/var/lib/hplip-printer-app \
      -v /dev/bus/usb:/dev/bus/usb:ro \
      --device-cgroup-rule='c 189:* rmw' \
      openprinting/hplip-printer-app:latest
```

- `PORT` is an optional environment variable used to start the printer-app on a specified port. If not provided, it will start on the default port 8000 or, if port 8000 is busy, on 8001 and so on.
- **The container must be started in `--network host` mode** to allow the Printer-Application instance inside the container to access and discover printers available in the local network where the host system is in.
- Alternatively using the internal network of the Docker instance (`-p <port>:8000` instead of `--network host -e PORT=<port>`) only gives access to local printers running on the host system itself.
- `-v hplip-printer-app:/var/lib/hplip-printer-app` maps a volume for persistent storage.
- The following volume and device settings are crucial for USB printer access:
  - `-v /dev/bus/usb:/dev/bus/usb:ro` mounts the host's USB device directory read-only inside the container for USB printer access.
  - `--device-cgroup-rule='c 189:* rmw'` allows the container to read, write, and mknod to USB devices.

### Setting Up and Running hplip-printer-app locally

#### Prerequisites

**Docker Installed**: Ensure Docker is installed on your system. You can download it from the [official Docker website](https://www.docker.com/get-started) or from the Snap Store:
```sh
  sudo snap install docker
```

**Rockcraft**: Rockcraft should be installed. You can install Rockcraft using the following command:
```sh
  sudo snap install rockcraft --classic
```

**Skopeo**: Skopeo should be installed to compile `*.rock` files into Docker images. It comes bundled with Rockcraft, so no separate installation is required.

#### Step-by-Step Guide

**Build hplip-printer-app rock**

The first step is to build the Rock from the `rockcraft.yaml`. This image will contain all the configurations and dependencies required to run hplip-printer-app.

Open your terminal and navigate to the directory containing your `rockcraft.yaml`, then run the following command:

```sh
  rockcraft pack -v
```

**Compile to Docker Image**

Once the rock is built, you need to compile docker image from it.

```sh
  sudo rockcraft.skopeo --insecure-policy copy oci-archive:<rock_image> docker-daemon:hplip-printer-app:latest
```

Create a Docker volume:
```sh
  sudo docker volume create hplip-printer-app
```

**Run the hplip-printer-app Docker Container**

```sh
  sudo docker run -d \
      --name hplip-printer-app \
      --network host \
      -e PORT=<port> \
      -v hplip-printer-app:/var/lib/hplip-printer-app \
      -v /dev/bus/usb:/dev/bus/usb:ro \
      --device-cgroup-rule='c 189:* rmw' \
      hplip-printer-app:latest
```
- `PORT` is an optional environment variable used to start the printer-app on a specified port. If not provided, it will start on the default port 8000 or, if port 8000 is busy, on 8001 and so on.
- **The container must be started in `--network host` mode** to allow the Printer-Application instance inside the container to access and discover printers available in the local network where the host system is in.
- Alternatively using the internal network of the Docker instance (`-p <port>:8000` instead of `--network host -e PORT=<port>`) only gives access to local printers running on the host system itself.
- `-v hplip-printer-app:/var/lib/hplip-printer-app` maps a volume for persistent storage.
- The following volume and device settings are crucial for USB printer access:
  - `-v /dev/bus/usb:/dev/bus/usb:ro` mounts the host's USB device directory read-only inside the container for USB printer access.
  - `--device-cgroup-rule='c 189:* rmw'` allows the container to read, write, and mknod to USB devices.

#### Setting up

Enter the web interface

```sh
http://localhost:<port>/
```

Use the web interface to add a printer. Supply a name, select the
discovered printer, then select make and model. Also set the installed
accessories, loaded media and the option defaults. If the printer is a
PostScript printer, accessory configuration and option defaults can
also often get polled from the printer.

<!-- Begin Included Components -->
## Included Components
  - pappl v1.4.9
  - qpdf v11.10.1
  - ghostscript ghostpdl-10.06.0rc1_test001
  - cups v2.4.14
  - libcupsfilters 2.1.1
  - libppd 2.1.1
  - pyppd release-1-1-0
  - hplip debian/3.26.4+dfsg0-3
<!-- End Included Components -->

## BUILDING WITHOUT PACKAGING OR INSTALLATION

You can also do a "quick-and-dirty" build without snapping and without
needing to install [PAPPL](https://www.msweet.org/pappl),
[cups-filters 2.x](https://github.com/OpenPrinting/cups-filters), and
[pappl-retrofit](https://github.com/OpenPrinting/pappl-retrofit) into
your system. You need a directory with the latest GIT snapshot of
PAPPL, the latest GIT snapshot of cups-filters, and the latest GIT
snapshot of pappl-retrofit (master branches of each). They all need to
be compiled (`./autogen.sh; ./configure; make`), installing not
needed. Also install the header files of all needed libraries
(installing "libcups2-dev" should do it).

In the directory with hplip-printer-app.c run the command line

```
gcc -o hplip-printer-app hplip-printer-app.c hplip-plugin-verify.c hplip-download-policy.c $PAPPL_SRC/pappl/libpappl.a $CUPS_FILTERS_SRC/.libs/libppd.a $CUPS_FILTERS_SRC/.libs/libcupsfilters.a $PAPPL_RETROFIT_SRC/.libs/libpappl-retrofit.a -ldl -lpthread  -lppd -lcups -lavahi-common -lavahi-client -lgnutls -ljpeg -lpng16 -ltiff -lz -lm -lusb-1.0 -lpam -lqpdf -lstdc++ -I. -I$PAPPL_SRC/pappl -I$CUPS_FILTERS_SRC/ppd -I$CUPS_FILTERS_SRC/cupsfilters -I$PAPPL_RETROFIT_SRC/pappl/retrofit -L$CUPS_FILTERS_SRC/.libs/ -L$PAPPL_RETROFIT_SRC/.libs/
```

There is also a Makefile, but this needs PAPPL, cups-filters 2.x, and
pappl-retrofit to be installed into your system.

Run

```
./hplip-printer-app --help
```

When running the non-snapped version, by default, PPD files are
searched for in

```
/usr/share/ppd/
/usr/lib/cups/driver/
/var/lib/hplip-printer-app/ppd/
```

You can set the `PPD_PATHS` environment variable to search other
places instead:

```
PPD_PATHS=/path/to/my/ppds:/my/second/place ./hplip-printer-app server
```

Simply put a colon-separated list of any amount of paths into the
variable. Creating a wrapper script is recommended.

Note that only PPD files for the `hpcups` driver of HPLIP are
considred, other PPD files are ignored.

Printers are only discovered via the `hp` backend of HPLIP (USB) or
the `hp-probe` utility of HPLIP (network). For the latter a wrapper
script named `HP` is included which makes the utility be used like a
CUPS backend (discovery mode only). This especially makes only HP and
Apollo printers being discovered. Printers from other manufacturers
are not supported.

The `HP` wrapper needs nothing but a POSIX shell and the Python runtime
of `hp-probe` itself, so it also works as non-root in the OCI image and
in the Snap, and it announces every discovered printer with its standard
`hp:/net/...` device URI. Its parsing of the `hp-probe` output and the
device records it emits are tested by `tests/hp-discovery.sh`, which
drives it with synthetic discovery responses.

Jobs are filtered through `hpcups` and send to the printer via the
`hp` backend (both USB and network).

The standard (not HPLIP's) backends provided as alternative in this
Printer Application are CUPS' backends and not PAPPL's, meaning that
for USB printers CUPS' USB quirk workarounds for compatibility
problems are used, network printers can also be used with IPP, IPPS,
and LPD protocols, and SNMP printer discovery is configurable.

USB Quirk rules in `/usr/share/cups/usb` and the `/etc/cups/snmp.conf`
file can get edited if needed.

Make sure you have HPLIP installed and if you want to use standard
backends, CUPS (at least its backends).

You also need Ghostscript to print PDF or PostScript jobs.

For access to the test page `testpage.ps` use the TESTPAGE_DIR
environment variable:

```
TESTPAGE_DIR=`pwd` PPD_PATHS=/path/to/my/ppds:/my/second/place ./hplip-printer-app server
```

or for your own creation of a test page (PostScript, PDF, PNG, JPEG,
Apple Raster, PWG Raster):

```
TESTPAGE=/path/to/my/testpage/my_testpage.ps PPD_PATHS=/path/to/my/ppds:/my/second/place ./hplip-printer-app server
```


## TESTS

The proprietary plugin is downloaded while the web admin request which
asked for it is waiting for an answer, so every plugin transfer is
bounded. By default the connection has to come up within 30 seconds, the
whole transfer has to finish within 900 seconds, and a transfer which
stays below 1024 bytes per second for 60 seconds is aborted. A download
which hits one of these bounds removes its temporary file and reports
what went wrong on the plugin page of the web interface, instead of
holding the request open and instead of leaving a partial file where the
installer could pick it up.

The bounds can be changed for an unusually slow mirror with the
environment variables `HPLIP_PLUGIN_CONNECT_TIMEOUT`,
`HPLIP_PLUGIN_TIMEOUT`, `HPLIP_PLUGIN_STALL_LIMIT` and
`HPLIP_PLUGIN_STALL_TIME` (seconds, seconds, bytes per second, seconds).
A value which is not a positive whole number, or which is out of range,
is ignored and logged, and the built-in value is used for it instead.

The tests for this need neither PAPPL nor libcurl: the bounds live in
`hplip-download-policy.c`, which the tests build against a stand-in for
libcurl's header, and the stalled endpoint the failure case is
demonstrated on is a local server started by `tests/stall-server.py`.

```
tests/run-download-policy-tests.sh
```

The same script compiles the module against libcurl's own header when it
is installed, which is what catches a bound named wrongly. To point it at
libcurl headers unpacked somewhere other than the system include
directory, set `CURL_INCLUDE_DIR`.

`.github/workflows/plugin-download-bounds.yml` runs these tests; the
application itself is compiled with the bounds in place by the merge-queue
image build (`just verify`).


## LEGAL STUFF

The HPLIP Printer Application is Copyright © 2020 by Till Kamppeter.

It is derived from the HP PCL Printer Application, a first working model of
a raster Printer Application using PAPPL. It is available here:

https://github.com/michaelrsweet/hp-printer-app

The HP PCL Printer Application is Copyright © 2019-2020 by Michael R Sweet.

This software is licensed under the Apache License Version 2.0 with an exception
to allow linking against GPL2/LGPL2 software (like older versions of CUPS).  See
the files "LICENSE" and "NOTICE" for more information.

### HP plugin signature verification

Plugin downloads are verified before the self-extracting archive is executed.
The verifier imports the packaged `/usr/share/hplip/signing-key.asc` into
`$STATE_DIR/plugin-gnupg` (default `/var/lib/hplip-printer-app/plugin-gnupg`).
This directory must belong to the application's runtime UID and have mode
0700. Its parent must already exist and be writable by that UID on the
persistent state volume. No root privileges or install-time keyserver access
are needed for verification. Snap uses its existing persistent `STATE_DIR`
and the key inside the Snap.

The OCI image and the Rock and Snap recipes already install Debian HPLIP's
public key. Native
packagers must also supply it; `HPLIP_SIGNING_KEY` and `HPLIP_APP_STATE_DIR` are
Makefile overrides for nonstandard paths. The currently pinned primary
fingerprint is `4ABA2F66DBD5A95894910E0673D770CDA59047B9`, as documented in
[upstream's plugin verifier](https://github.com/OpenPrinting/hplip-printer-app/blob/master/hplip-printer-app.c).
An unrelated key in the persistent keyring cannot authorize a plugin. Missing
keys, changed payloads/signatures, expired or revoked signatures/keys, and GPG
errors fail closed before extraction and installation. User GPG configuration
is ignored. A future HP key rotation requires reviewing HP's authorization and
updating the fingerprint and packaged key together; do not disable verification.

Run the cryptographic regression tests with a C compiler, Python 3, GnuPG and
`gpgconf` installed:

```sh
python3 tests/test-plugin-verification.py
```

To include official bytes already downloaded from HP's OpenPrinting mirror:

```sh
HP_PLUGIN=/path/to/hplip-3.22.10-plugin.run \
HP_PLUGIN_SIGNATURE=/path/to/hplip-3.22.10-plugin.run.asc \
HP_SIGNING_KEY=/usr/share/hplip/signing-key.asc \
python3 tests/test-plugin-verification.py
```

The dedicated CI workflow downloads these test inputs, then runs verification
without networking as UID 65532. It never executes the archive or publishes
its proprietary bytes. These tests cover the production verifier; they do not
replace OCI web-consent, restart, and print-to-socket-sink integration testing
for the runtime tracked in issues #3 and #9. Physical output needs hardware.
