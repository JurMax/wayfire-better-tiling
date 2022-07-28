pkgname=wayfire-better-tiling
pkgver=0.1
pkgrel=1
pkgdesc="Tiling window manager for Wayfire."
arch=('x86_64')
url="https://github.com/JurMax/wayfire-better-tiling"
license=('MIT')
depends=('wayfire')
makedepends=('git' 'meson' 'ninja' 'cmake')
provides=('wayfire-better-tiling')
conflicts=("$pkgname")
replaces=()
options=()

source=()
sha256sums=()

build() {
    cd ../
    meson build --buildtype=release
    ninja -C build
}


package() {
    cd ../
    DESTDIR="$pkgdir/" ninja -C build install
}
