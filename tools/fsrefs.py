#!/usr/bin/env drgn

import argparse
import os
import sys
import typing
from typing import Any, Callable, Optional, Sequence, Union

from drgn import FaultError, Object, Program
from drgn.helpers.linux.fs import (
    d_path,
    fget,
    for_each_file,
    for_each_mount,
    inode_path,
    mount_dst,
)
from drgn.helpers.linux.mm import for_each_vma
from drgn.helpers.linux.pid import find_task, for_each_task


class warn_on_fault:
    def __init__(self, message: Union[str, Callable[[], str]]) -> None:
        self._message = message

    def __enter__(self) -> None:
        pass

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> bool:
        if exc_type is not None and issubclass(exc_type, FaultError):
            message = (
                self._message if isinstance(self._message, str) else self._message()
            )
            if message:
                print(
                    f"warning: fault while {message}, possibly due to race; results may be incomplete",
                    file=sys.stderr,
                )
            return True
        return False


ignore_fault = warn_on_fault("")


format_args = {
    "dereference": False,
    "symbolize": False,
}


if typing.TYPE_CHECKING:

    class Visitor(typing.Protocol):  # novermin
        def visit_file(self, file: Object) -> Optional[str]:
            ...

        def visit_inode(self, inode: Object) -> Optional[str]:
            ...

        def visit_path(self, path: Object) -> Optional[str]:
            ...


class InodeVisitor:
    def __init__(self, inode: Object) -> None:
        self._inode = inode.read_()

    def visit_file(self, file: Object) -> Optional[str]:
        if file.f_inode != self._inode:
            return None
        return file.format_(**format_args)

    def visit_inode(self, inode: Object) -> Optional[str]:
        if inode != self._inode:
            return None
        return inode.format_(**format_args)

    def visit_path(self, path: Object) -> Optional[str]:
        if path.dentry.d_inode != self._inode:
            return None
        return path.format_(**format_args)


class SuperBlockVisitor:
    def __init__(self, sb: Object) -> None:
        self._sb = sb.read_()

    def visit_file(self, file: Object) -> Optional[str]:
        if file.f_inode.i_sb != self._sb:
            return None
        match = file.format_(**format_args)
        with ignore_fault:
            match += " " + os.fsdecode(d_path(file.f_path))
        return match

    def visit_inode(self, inode: Object) -> Optional[str]:
        if inode.i_sb != self._sb:
            return None
        match = inode.format_(**format_args)
        with ignore_fault:
            path = inode_path(inode)
            if path:
                match += " " + os.fsdecode(path)
        return match

    def visit_path(self, path: Object) -> Optional[str]:
        if path.mnt.mnt_sb != self._sb:
            return None
        match = path.format_(**format_args)
        with ignore_fault:
            match += " " + os.fsdecode(d_path(path))
        return match


def visit_tasks(
    prog: Program, visitor: "Visitor", *, check_mounts: bool, check_tasks: bool
) -> None:
    check_mounts = check_mounts and isinstance(visitor, SuperBlockVisitor)
    if check_mounts:
        init_mnt_ns = prog["init_task"].nsproxy.mnt_ns
        checked_mnt_ns = {0}
    with warn_on_fault("iterating tasks"):
        for task in for_each_task(prog):
            cached_task_id = None

            def task_id() -> str:
                nonlocal cached_task_id
                if cached_task_id is None:
                    pid = task.pid.value_()
                    comm = os.fsdecode(task.comm.string_())
                    cached_task_id = f"pid {pid} ({comm})"
                return cached_task_id

            def task_fault_warning() -> str:
                try:
                    return f"checking {task_id()}"
                except FaultError:
                    return "checking task"

            with warn_on_fault(task_fault_warning):
                files: Optional[Object] = task.files.read_()
                fs: Optional[Object] = task.fs.read_()
                mm: Optional[Object] = task.mm.read_()

                # If this task is not the thread group leader, don't bother
                # checking it again unless it has its own context.
                group_leader = task.group_leader.read_()
                if task != group_leader:
                    if files and files == group_leader.files:
                        files = None
                    if fs and fs == group_leader.fs:
                        fs = None
                    if mm and mm == group_leader.mm:
                        mm = None

                if check_mounts:
                    nsproxy = task.nsproxy.read_()
                    if nsproxy:
                        mnt_ns = nsproxy.mnt_ns.read_()
                        if mnt_ns.value_() not in checked_mnt_ns:
                            for mount in for_each_mount(mnt_ns):
                                with ignore_fault:
                                    if mount.mnt.mnt_sb == visitor._sb:  # type: ignore [attr-defined]
                                        if mnt_ns == init_mnt_ns:
                                            mnt_ns_note = ""
                                        else:
                                            mnt_ns_note = f" (mount namespace {mnt_ns.ns.inum.value_()})"
                                        print(
                                            f"mount {os.fsdecode(mount_dst(mount))}{mnt_ns_note} "
                                            f"{mount.format_(**format_args)}"
                                        )

                            checked_mnt_ns.add(mnt_ns.value_())

                if check_tasks:
                    if files:
                        for fd, file in for_each_file(task):
                            with ignore_fault:
                                match = visitor.visit_file(file)
                                if match:
                                    print(f"{task_id()} fd {fd} {match}")

                    if fs:
                        with ignore_fault:
                            match = visitor.visit_path(fs.root.address_of_())
                            if match:
                                print(f"{task_id()} root {match}")
                        with ignore_fault:
                            match = visitor.visit_path(fs.pwd.address_of_())
                            if match:
                                print(f"{task_id()} cwd {match}")

                    if mm:
                        exe_file = mm.exe_file.read_()
                        if exe_file:
                            match = visitor.visit_file(exe_file)
                            if match:
                                print(f"{task_id()} exe {match}")

                        for vma in for_each_vma(mm):
                            with ignore_fault:
                                file = vma.vm_file.read_()
                                if file:
                                    match = visitor.visit_file(file)
                                    if match:
                                        print(
                                            f"{task_id()} vma {hex(vma.vm_start)}-{hex(vma.vm_end)} {match}"
                                        )


def hexint(x: str) -> int:
    return int(x, 16)


def main(prog: Program, argv: Sequence[str]) -> None:
    parser = argparse.ArgumentParser(
        description="find what is referencing a filesystem object"
    )

    parser.add_argument(
        "-L",
        "--dereference",
        action="store_true",
        help="if the given path is a symbolic link, follow it",
    )

    object_group = parser.add_argument_group(
        title="filesystem object selection"
    ).add_mutually_exclusive_group(required=True)
    object_group.add_argument(
        "--inode", metavar="PATH", help="find references to the inode at the given path"
    )
    object_group.add_argument(
        "--inode-pointer",
        metavar="ADDRESS",
        type=hexint,
        help="find references to the given struct inode pointer",
    )
    object_group.add_argument(
        "--super-block",
        metavar="PATH",
        help="find references to the filesystem (super block) containing the given path",
    )
    object_group.add_argument(
        "--super-block-pointer",
        metavar="ADDRESS",
        type=hexint,
        help="find references to the given struct super_block pointer",
    )

    CHECKS = [
        "mounts",
        "tasks",
    ]
    check_group = parser.add_argument_group(
        title="check selection"
    ).add_mutually_exclusive_group()
    check_group.add_argument(
        "--check",
        choices=CHECKS,
        action="append",
        help="only check for references from the given source; may be given multiple times (default: all)",
    )
    check_group.add_argument(
        "--no-check",
        choices=CHECKS,
        action="append",
        help="don't check for references from the given source; may be given multiple times",
    )

    args = parser.parse_args(argv)

    visitor: "Visitor"
    if args.inode is not None:
        fd = os.open(args.inode, os.O_PATH | (0 if args.dereference else os.O_NOFOLLOW))
        try:
            visitor = InodeVisitor(fget(find_task(prog, os.getpid()), fd).f_inode)
        finally:
            os.close(fd)
    elif args.inode_pointer is not None:
        visitor = InodeVisitor(Object(prog, "struct inode *", args.inode_pointer))
    elif args.super_block is not None:
        fd = os.open(
            args.super_block, os.O_PATH | (0 if args.dereference else os.O_NOFOLLOW)
        )
        try:
            visitor = SuperBlockVisitor(
                fget(find_task(prog, os.getpid()), fd).f_inode.i_sb
            )
        finally:
            os.close(fd)
    elif args.super_block_pointer is not None:
        visitor = SuperBlockVisitor(
            Object(prog, "struct super_block *", args.super_block_pointer)
        )
    else:
        assert False

    if args.check:
        enabled_checks = set(args.check)
    else:
        enabled_checks = set(CHECKS)
        if args.no_check:
            enabled_checks -= set(args.no_check)

    if "mounts" in enabled_checks or "tasks" in enabled_checks:
        visit_tasks(
            prog,
            visitor,
            check_mounts="mounts" in enabled_checks,
            check_tasks="tasks" in enabled_checks,
        )


if __name__ == "__main__":
    prog: Program
    main(prog, sys.argv[1:])
