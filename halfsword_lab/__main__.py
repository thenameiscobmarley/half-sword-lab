import argparse

from .app import LabApp


def main() -> None:
    ap = argparse.ArgumentParser(prog="half-sword-lab", description="Trainer, save editor and mod lab for Half Sword")
    ap.add_argument("--demo", action="store_true", help="explore with fake data, no game needed")
    ap.add_argument("--saves-only", action="store_true",
                    help="save editor edition: only ever touches your save files (and backs them up)")
    args = ap.parse_args()
    LabApp(demo=args.demo, saves_only=args.saves_only).run()


if __name__ == "__main__":
    main()
