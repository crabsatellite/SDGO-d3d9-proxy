"""Require a truthful, explicit model disclosure in PR text; never execute it."""
import json
import os
from pathlib import Path
import re
import sys


def check(body):
    text=re.sub(r'<!--.*?-->','',(body or '').replace('\r\n','\n').replace('\r','\n'),flags=re.S)
    match=re.search(r'^[ \t]*(?:-[ \t]*)?AI models[ \t]*:[ \t]*([^\r\n]*)$',text,re.I|re.M)
    if not match:
        return False,'PR must include AI models: followed by the actual model name/version, None, or an explicit Unknown.'
    value=match[1].strip().strip('`')
    if not value or value.casefold() in ('todo','tbd','n/a','model','[model]','<model>','待填写','请填写','unknown model name here') or value.startswith(('<!--','<','[')):
        return False,'Replace the AI models placeholder with the actual model disclosure; do not infer or invent a model.'
    return True,value


def main():
    event=json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
    valid,detail=check(event.get('pull_request',{}).get('body',''))
    print(('AI disclosure present: ' if valid else 'AI disclosure missing: ')+detail)
    return 0 if valid else 1


if __name__=='__main__':sys.exit(main())
