# beamfs - resilient filesystem

beamfs is an EM-resilient Linux filesystem with RS(255,239) forward error
correction, targeting mainline Linux. It is the implementation successor
to FTRFS, extending the original design with a formal recovery calculus
and a soundness theorem.

Active development continues on a private branch toward publication.
This repository will be updated with the full release at that time.

## Published artifacts

- beamfs v2 paper: [DOI 10.5281/zenodo.19886192](https://doi.org/10.5281/zenodo.19886192)
- emufi v1 paper (EM fault injector): [DOI 10.5281/zenodo.19885777](https://doi.org/10.5281/zenodo.19885777)
- FTRFS v1 (predecessor): [DOI 10.5281/zenodo.19824442](https://doi.org/10.5281/zenodo.19824442)

## Lineage

The FTRFS name and original concept originates in:

> Fuchs, C.M., Langer, M., Trinitis, C. (2015).
> *FTRFS: A Fault-Tolerant Radiation-Robust Filesystem for Space Use.*
> ARCS 2015, Lecture Notes in Computer Science, vol 9017. Springer.
> DOI: <https://doi.org/10.1007/978-3-319-16086-3_8>

FTRFS v1 (Desbrieres, 2026) is an independent open-source realization
of the Fuchs et al. design with contemporary Linux kernel infrastructure.
beamfs extends FTRFS with a formal recovery operator and a soundness
theorem.

## License

GPL-2.0-only (kernel module + userspace tools), CC-BY-4.0 (papers).
