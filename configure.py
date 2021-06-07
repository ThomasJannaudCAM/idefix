#!/usr/bin/env python
from __future__ import print_function

import argparse
from collections import defaultdict
import os
import re
import sys
import platform


# CPU_ARCHS and GPU_ARCHS are alphabetically ordered
CPU_ARCHS = frozenset(
    (
        "BDW",
        "EPYC",
        "HSW",
        "SKX",
        "WSM",
    ),
)

GPU_ARCHS = frozenset(
    (
        "Kepler30",
        "Maxwell50",
        "Pascal60",
        "Pascal61",
        "Turing75",
        "Volta70",
        "Volta72",
    ),
)

KNOWN_ARCHS = {"CPU": CPU_ARCHS, "GPU": GPU_ARCHS}
DEFAULT_ARCHS = {"CPU": "BDW", "GPU": "Pascal60"}

_GPU_FLAG_DEPRECATION_MESSAGE = (
    "The -gpu flag is deprecated. Using it will raise an error in a future release. "
    "Please explicitly request a GPU architecture via the -arch argument."
)


def _add_parser_args(parser):
    parser.add_argument(
        "directory", nargs="?", default=os.getcwd(), help="target directory",
    )

    parser.add_argument("-mhd", action="store_true", help="enable MHD")

    parser.add_argument(
        "-gpu",
        action="store_true",
        help=_GPU_FLAG_DEPRECATION_MESSAGE,
    )

    parser.add_argument("-cxx", help="override default compiler")

    parser.add_argument(
        "-arch",
        nargs="+",
        dest="archs",
        choices=CPU_ARCHS.union(GPU_ARCHS),
        help="target Kokkos architecture (accepts up to one CPU and up to one GPU archs)",
    )
    parser.add_argument(
        "-openmp",
        help="enable OpenMP parallelism (not available with GPU architectures)",
        action="store_true",
    )
    parser.add_argument("-mpi", action="store_true", help="enable MPI parallelism")


def is_gpu_requested(requested_archs):
    if requested_archs is None:
        return False
    return any((a in GPU_ARCHS for a in requested_archs))


def parse_archs(requested_archs):
    # parse architectures:
    # at most 2 can be specified by the user, but only one for each arch type (CPU, GPU)
    if requested_archs is None:
        return DEFAULT_ARCHS
    if len(requested_archs) > 2:
        raise ValueError(
            "Error: received more than two architectures ({}).".format(
                ", ".join(requested_archs),
            ),
        )
    selected_archs = DEFAULT_ARCHS.copy()

    for arch_type, archs in KNOWN_ARCHS.items():
        vals = sorted(list(archs.intersection(set(requested_archs))))
        if not vals:
            continue
        if len(vals) > 1:
            raise ValueError(
                "Error: received more than one {} architecture ({}).".format(
                    arch_type, ", ".join(vals),
                ),
            )

        selected_archs[arch_type] = vals[0]
    return selected_archs


def _get_sed():
    # Build a sed command which is compatible with the current platofm (BSD and GNU diverge on that)
    sed = ""
    if platform.system() == 'Darwin':
        sed = "sed -i '' "
    else:
        sed = "sed -i"

    return sed


def _get_makefile_options(
    archs,
    use_gpu,
    cxx,
    openmp,
    mpi,
    mhd,
    sed,
):
    # using a default dict to allow setting key value pairs as
    # >>> options[key] += value

    options = defaultdict(str)
    options["cxxflags"] = "-O3"
    options["sed-command"] = sed

    if use_gpu:
        options["extraLine"] = '\nKOKKOS_CUDA_OPTIONS = "enable_lambda"'
        options["cxx"] = "${KOKKOS_PATH}/bin/nvcc_wrapper"
        options["kokkosDevices"] = '"Cuda"'
        options["kokkosArch"] = "{CPU},{GPU}".format(**archs)

        # Enforce backend compiler for nvcc
        nvcc = "\nexport NVCC_WRAPPER_DEFAULT_COMPILER = {}"
        if cxx:
            options["extraLine"] += nvcc.format(cxx)
        elif mpi:
            options["extraLine"] += nvcc.format("mpicxx")
    else:
        if cxx:
            options["cxx"] = cxx
        elif mpi:
            options["cxx"] = "mpicxx"
        else:
            options["cxx"] = "g++"

        options["kokkosArch"] = archs["CPU"]
        options["kokkosDevices"] = '"OpenMP"' if openmp else '"Serial"'

    if mpi:
        options["extraIncludeDir"] += " -I$(SRC)/dataBlock/mpi"
        options["extraVpath"] += ":$(SRC)/dataBlock/mpi"
        options["extraObj"] += " mpi.o"
        options["cxxflags"] += " -DWITH_MPI"

    if mhd:
        options["extraIncludeDir"] += " -I$(SRC)/hydro/MHDsolvers"
        options["extraVpath"] += ":$(SRC)/hydro/MHDsolvers"
        options["cxxflags"] += " -DMHD=YES"
    else:
        options["extraIncludeDir"] += " -I$(SRC)/hydro/HDsolvers"
        options["extraVpath"] += ":$(SRC)/hydro/HDsolvers"
        options["cxxflags"] += " -DMHD=NO"

    return options


def _write_makefile(
    directory,
    options,
):
    with open(os.path.join(os.environ["IDEFIX_DIR"], "Makefile.in")) as fh:
        data = fh.read()

    # apply subsitutions
    for key, val in options.items():
        data = data.replace(r"@{}@".format(key), val)

    # cleanup unused place holders
    data = re.sub(r"@.+@", "", data)

    with open(os.path.join(directory, "Makefile"), "w") as fh:
        fh.write(data)


def _get_report(
    archs,
    openmp,
    mpi,
    mhd,
    makefile_options,
):
    def status(name, flag):
        prefix = "en" if flag else "dis"
        return "{}: {}abled".format(name, prefix)

    report_lines = []
    report_lines += [
        "-----------------------------------------------------------",
        "Idefix succesfully configured with the following options:",
        "",
        status("MHD", mhd),
        "Compiler: {}".format(makefile_options["cxx"]),
        status("MPI", mpi),
    ]

    selected_archs = parse_archs(archs)
    arch_type = "GPU" if is_gpu_requested(archs) else "CPU"
    report_lines += [
        "Execution target: {}".format(arch_type),
        "Target architecture: {}".format(selected_archs[arch_type]),
    ]
    if arch_type == "CPU":
        report_lines.append(status("OpenMP", openmp))

    report_lines += [
        "Cflags: {}".format(makefile_options["cxxflags"]),
        "-----------------------------------------------------------",
    ]

    return "\n".join(report_lines)


def main(argv=None):

    if "IDEFIX_DIR" not in os.environ:
        print(
            "Error: IDEFIX_DIR environment variable is not defined.",
            file=sys.stderr,
        )
        return 1

    parser = argparse.ArgumentParser("setup")
    _add_parser_args(parser)

    args = parser.parse_args(argv)

    try:
        selected_archs = parse_archs(args.archs)
    except ValueError as err:
        print(err, file=sys.stderr)
        return 1

    use_gpu = is_gpu_requested(args.archs)
    if args.gpu:
        print("Warning: " + _GPU_FLAG_DEPRECATION_MESSAGE, file=sys.stderr)
        if not use_gpu:
            print(
                "Warning: -gpu flag was received, but no GPU architecture was specified. "
                "Defaulting to %s" % selected_archs["GPU"],
                file=sys.stderr,
            )

    if args.openmp and use_gpu:
        print("Warning: with a GPU arch, -openmp flag is ignored.", file=sys.stderr)
        args.openmp = False

    mysed = _get_sed()

    makefile_options = _get_makefile_options(
        archs=selected_archs,
        use_gpu=use_gpu,
        cxx=args.cxx,
        openmp=args.openmp,
        mpi=args.mpi,
        mhd=args.mhd,
        sed=mysed,
    )
    try:
        _write_makefile(args.directory, makefile_options)
    except OSError:
        filename = os.path.join(args.directory, "Makefile")
        print("Error: could not write to {}".format(filename), file=sys.stderr)
        return 1

    report = _get_report(
        archs=args.archs,
        openmp=args.openmp,
        mpi=args.mpi,
        mhd=args.mhd,
        makefile_options=makefile_options,
    )
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
