# vendor/ — third-party material, versioned on purpose

## What is here

`INE5424-x86_64-starter-6.15.5.tar.gz` — the instructor's starter: the 6.15.5
`PREEMPT_RT` `bzImage` kernel, an `initramfs.cpio` with static BusyBox, and the
three scripts the fleet uses (`run-vm.sh`, `install-app.sh`,
`repack-initramfs.sh`). 15 MB, with the `.sha256` that came with it.

## Why it is versioned

The assignment requires that a `make` at the root compile and run the whole
evaluation. A Makefile pointing at `$HOME/work/so2/...` only satisfies that on
the machine of whoever wrote it — on any clean clone, `make image` dies at the
first command. Having the starter inside the repository is what makes the
evaluated clone self-contained.

The price is 15 MB permanently in the git history. That is deliberate: the blob
is unique, it never changes, and the alternative (downloading during `make`)
trades a path dependency for a network dependency — worse on presentation day.

## Rule of use: nobody touches anything in here

`repack-initramfs.sh` **writes into the tree it lives in**. Running it in place
would dirty the original material — which is why the "work on a copy" rule
existed back when the starter lived in `~/work/so2/`.

Now the rule is in the Makefile and does not depend on discipline: the `starter`
target unpacks the `.tar.gz` into `build/vm/`, checking the sha256 first, and
`install-app.sh` always runs in `build/vm/`. The tarball here is never touched,
and a `make clean-vm` restores the image to factory state.

    make starter     # checks the sha256 and unpacks into build/vm/
    make clean-vm    # deletes build/vm/ (the next make starter rebuilds it)

## Provenance

Distributed by the instructor for INE5424 2026/2 Practical Class 1. The guide
that ships with the bundle is in `doc/practical_class_1_guide.md`; the project
proposal is in `doc/full_assignment.pdf`. The raw Ethernet solution deliberately
does not come with the starter — that is what this repository implements.
