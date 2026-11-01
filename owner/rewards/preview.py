"""Headless developer example runner; the user interface is the Mod Manager."""
import argparse
import json
from pathlib import Path
from . import model


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario',type=Path,required=True)
    args=parser.parse_args()
    value=json.loads(args.scenario.read_text(encoding='utf-8-sig'))
    print(json.dumps(model.preview(model.defaults(),value['context'],value['stats']),ensure_ascii=False,indent=2))


if __name__=='__main__':main()
