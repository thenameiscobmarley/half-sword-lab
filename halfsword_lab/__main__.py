import argparse

from .app import LabApp


def main() -> None:
    ap = argparse.ArgumentParser(prog="half-sword-lab", description="Terminal trainer and mod lab for Half Sword")
    ap.add_argument("--demo", action="store_true", help="explore the GUI with fake data, no game needed")
    args = ap.parse_args()
    LabApp(demo=args.demo).run()


if __name__ == "__main__":
    main()
