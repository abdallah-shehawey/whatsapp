pkgname=whatsapp
pkgver=1.0.11
pkgrel=1
pkgdesc='A small WhatsApp Web client for GTK4 and WebKitGTK 6'
arch=('x86_64')
url='https://github.com/abdallah-shehawey/whatsapp'
license=('GPL-3.0-or-later')
depends=('gtk4' 'webkitgtk-6.0' 'glib2' 'hicolor-icon-theme')
makedepends=('gcc' 'make' 'python')
source=("https://github.com/abdallah-shehawey/whatsapp/archive/refs/tags/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
  cd "${srcdir}/${pkgname}-${pkgver}"
  make
}

package() {
  cd "${srcdir}/${pkgname}-${pkgver}"
  make DESTDIR="${pkgdir}" PREFIX=/usr install
  install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
  install -Dm644 README.md "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}

# vim: set ft=sh:

# Maintainer: Abdallah Shehawey <shehawey9@gmail.com>
# The package builds the same source tree used by the RPM and DEB releases.
# For a local checkout, replace source=() with the checkout path or run makepkg
# from the tagged source archive downloaded by GitHub.
