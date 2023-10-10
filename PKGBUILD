pkgname=wayfire-better-tile
pkgver=0.1
pkgrel=1
pkgdesc="Tiling window manager for Wayfire."
arch=('x86_64')
url="https://github.com/JurMax/wayfire-better-tile"
license=('MIT')
depends=('wayfire')
makedepends=('git' 'meson' 'ninja' 'cmake')
provides=('wayfire-better-tile')
conflicts=("$pkgname")
replaces=()
options=()

source=()
sha256sums=()

build() {
    cd ../
    meson build --prefix=/usr --buildtype=release
    ninja -C build
}


package() {
    cd ../
    DESTDIR="$pkgdir/" ninja -C build install
}
