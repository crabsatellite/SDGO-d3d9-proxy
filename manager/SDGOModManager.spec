# Build from the repository; all inputs are generic code/contracts, never owner state.
from pathlib import Path
import os
import sys
from importlib import metadata
root = Path(SPECPATH).parent
library = Path(os.environ['SDGO_MANAGER_SERVICES'])
data = [(str(root/'manager/static'),'manager/static'),
        (str(root/'manager/repository.json'),'manager'),
        (str(root/'manager/addons'),'manager/addons'),
        (str(root/'manager/engines/operators/resources'),'manager/engines/operators/resources'),
        (str(root/'manager/engines/operators/Install-Operator.ps1'),'manager/engines/operators'),
        (str(root/'manager/engines/capsules/install_capsule_client.ps1'),'manager/engines/capsules'),
        (str(root/'manager/engines/shop/bridge.ps1'),'manager/engines/shop'),
        (str(library),'manager/engines/shop'),
        (str(root/'owner/rewards/policy.example.json'),'owner/rewards'),
        (str(root/'owner/rewards/sql'),'owner/rewards/sql'),
        (str(root/'backend/install_ai_catalog_client.ps1'),'backend')]
data += [(str(path),str(path.parent.relative_to(root))) for path in sorted((root/'mods').glob('*/manager.json'))]
python_license=Path(sys.base_prefix)/'LICENSE.txt'
if python_license.is_file():data.append((str(python_license),'licenses/Python'))
for package in ('Pillow','pyodbc','PyInstaller','defusedxml'):
    try:distribution=metadata.distribution(package)
    except metadata.PackageNotFoundError:continue
    for item in distribution.files or []:
        if ('license' in str(item).lower() or 'copying' in str(item).lower()) and distribution.locate_file(item).is_file():
            data.append((str(distribution.locate_file(item)),'licenses/'+package))
a = Analysis([str(root/'tools/mod_manager.py')], pathex=[str(root)], datas=data,
             hiddenimports=['pyodbc','PIL.Image','PIL.ImageDraw'],
             excludes=['playwright','numpy','scipy','matplotlib','tkinter','pytest'], noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz,a.scripts,a.binaries,a.datas,[],name='SDGO Mod Manager',console=False,
          debug=False,strip=False,upx=False,disable_windowed_traceback=False)
