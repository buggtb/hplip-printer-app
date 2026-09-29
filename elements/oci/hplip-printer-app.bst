kind: script

build-depends:
  - fsdk-containers.bst:freedesktop-sdk.bst:bootstrap/bash.bst
  - fsdk-containers.bst:freedesktop-sdk.bst:bootstrap/coreutils.bst
  - fsdk-containers.bst:freedesktop-sdk.bst:components/oci-builder.bst
  - fsdk-containers.bst:freedesktop-sdk.bst:components/sed.bst
  - filename: printer-app/version.bst
    config:
      location: /metadata
  - filename: printer-app/runtime.bst
    config:
      location: /layer

config:
  commands:
    - |
      version="$(cat /metadata/VERSION)"
      install -d /layer/etc
      printf '%s\n' \
        'root:x:0:0:root:/root:/usr/bin/bash' \
        'nonroot:x:65532:65532:HPLIP Printer Application:/var/lib/hplip-printer-app:/usr/sbin/nologin' \
        >> /layer/etc/passwd
      printf '%s\n' 'root:x:0:' 'nonroot:x:65532:' >> /layer/etc/group
      rm -rf /layer/run/dbus /layer/run/avahi-daemon
      # Avahi's sample remote-login records do not describe this appliance.
      # Keep the shared base's avahi-printing element and PAPPL's dynamic IPP publication.
      rm -f /layer/etc/avahi/services/ssh.service \
        /layer/etc/avahi/services/sftp-ssh.service
      rm -rf /layer/usr/lib/debug /layer/usr/share/cups/ipptool
      rm -f /layer/usr/bin/cupsd /layer/usr/bin/ippeveprinter
      chmod 0777 /layer/run
      install -d -m 1777 /layer/tmp
      install -d -m 0777 /layer/var/lib/hplip-printer-app /layer/var/lib/hplip-printer-app/ppd
      if [ -f /layer/usr/share/dbus-1/system.conf ]; then
        sed -i '/<user>messagebus<\/user>/d; s/<policy user="root">/<policy user="nonroot">/g' /layer/usr/share/dbus-1/system.conf
      fi
      if [ -f /layer/etc/dbus-1/system.d/avahi-dbus.conf ]; then
        sed -i 's/<policy group="netdev">/<policy group="nonroot">/g; s/<policy user="root">/<policy user="nonroot">/g; s/<policy user="avahi">/<policy user="nonroot">/g' /layer/etc/dbus-1/system.d/avahi-dbus.conf
      fi
      cd "%{install-root}"
      build-oci <<EOF
      mode: oci
      gzip: disabled
      images:
        - os: linux
          architecture: "%{go-arch}"
          layer: /layer
          config:
            User: '65532:65532'
            Entrypoint: ['/usr/bin/catatonit', '--', '/usr/bin/bash', '/usr/libexec/hplip-printer-app/container-entrypoint']
            Env:
              - 'HOME=/var/lib/hplip-printer-app'
              - 'PATH=/usr/bin:/usr/sbin'
            Labels:
              'org.opencontainers.image.title': 'hplip-printer-app'
              'org.opencontainers.image.description': 'HPLIP print-only Printer Application built from freedesktop-sdk'
              'org.opencontainers.image.source': 'https://github.com/projectbluefin/hplip-printer-app'
              'org.opencontainers.image.licenses': 'Apache-2.0 AND GPL-2.0-or-later'
              'org.opencontainers.image.version': '${version}'
              'io.projectbluefin.fsdk.version': '26.08.1'
              'io.projectbluefin.fsdk.ref': 'b02b59ffe19a49a402f357fd5fcb1d552ebc50d7'
      EOF
