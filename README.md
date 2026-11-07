# Petrichor packages

Packages for [Petrichor OS](https://github.com/krspybacon/PetrichorOS). On a Petrichor machine:

    ppm update
    ppm install cowsay

`index` lists every package (name, version, size, SHA-256, dependencies); the `.ppk` files are the
packages. `ppm` verifies the size and SHA-256 before it installs anything. `packages/` holds the
sources, so you can rebuild with `make repo` in the main repository.
