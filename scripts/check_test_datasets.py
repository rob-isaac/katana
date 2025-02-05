#! /usr/bin/env python3
"""Check that all RDGs from test-datasets are present in TestDatasetsRDGs.cmake."""

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class RDGDir:
    """A RDG tracked under test-datasets."""

    path: Path

    @property
    def dir_name(self) -> str:
        """The directory under test-datasets where this dataset lives."""
        return self.path.name

    @property
    def var_name(self) -> str:
        """The name for the cmake variable for this dataset."""
        return "RDG_" + self.dir_name.replace("=", "_").upper()

    @property
    def cmake_definition(self) -> str:
        """The cmake definition for this dataset."""
        return f'rdg_dataset({self.var_name} "{self.dir_name}")'


if __name__ == "__main__":
    repo_root = subprocess.check_output("git rev-parse --show-toplevel".split()).decode().strip()

    repo_root = Path(repo_root)
    output_file = repo_root / "cmake/Modules/TestDatasetsRDGs.cmake"

    get_rdgs_cmd = [
        "find",
        repo_root / "external/test-datasets/rdg_datasets",
        "-mindepth",
        "1",
        "-maxdepth",
        "1",
        "-type",
        "d",
    ]
    rdgs_list = subprocess.check_output(get_rdgs_cmd).decode().split()

    rdgs = []
    for rdg_path in rdgs_list:
        rdgs.append(RDGDir(Path(rdg_path)))

    expected_file = ""
    expected_file += "### Generated by scripts/check_test_datasets.py --fix\n\n"
    for rdg in rdgs:
        expected_file += f"{rdg.cmake_definition}\n"

    if "--fix" in sys.argv:
        with open(output_file, "w") as output:
            output.write(expected_file)
    else:
        with open(output_file, "r") as actual_file:
            assert expected_file == actual_file.read(), "files differ"
