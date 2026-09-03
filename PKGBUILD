pkgname=whatsapp
pkgver=1.0.11
pkgrel=2
pkgdesc='A small WhatsApp Web client for GTK4 and WebKitGTK 6'
arch=('x86_64')
url='https://github.com/abdallah-shehawey/whatsapp'
license=('GPL-3.0-or-later')
depends=('gtk4' 'webkitgtk-6.0' 'glib2' 'hicolor-icon-theme')
makedepends=('gcc' 'make' 'python')
# The rpm marks this %config(noreplace) and the deb lists it in conffiles;
# without the same line here pacman silently overwrites it on every upgrade,
# so anyone who turned autostart off by editing it has it turned back on.
backup=('etc/xdg/autostart/io.github.shehawey.whatsapp.desktop')
source=("https://github.com/abdallah-shehawey/whatsapp/archive/refs/tags/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
  cd "${srcdir}/${pkgname}-${pkgver}"
  make
}

package() {
  cd "${srcdir}/${pkgname}-${pkgver}"
  make DESTDIR="${pkgdir}" PREFIX=/usr install

  # Autostart is shipped system-wide rather than written into a home
  # directory, so the package can cleanly remove it again. The Makefile's
  # `autostart` target writes into $HOME, which is not a package's to touch,
  # so the entry is staged here instead -- and without it Arch was the one
  # family whose install did not start hidden at login, against what the
  # README promises for every package.
  sed 's|@BINDIR@|/usr/bin|g' data/io.github.shehawey.whatsapp-autostart.desktop \
    > "${srcdir}/autostart.desktop"
  install -Dm644 "${srcdir}/autostart.desktop" \
    "${pkgdir}/etc/xdg/autostart/io.github.shehawey.whatsapp.desktop"

  install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
  install -Dm644 README.md "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}

# vim: set ft=sh:

# Maintainer: Abdallah Shehawey <shehawey9@gmail.com>
# The package builds the same source tree used by the RPM and DEB releases.
# For a local checkout, replace source=() with the checkout path or run makepkg
# from the tagged source archive downloaded by GitHub.
