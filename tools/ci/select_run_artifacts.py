"""Select the latest artifact per lane name from paginated workflow metadata.

A rerun can retain multiple artifacts with the same name. Downloading all of
those into one directory makes old failures overwrite the rerun's evidence.
The aggregate report itself is not lane input; original artifacts stay intact.
"""
import json
import sys

prefix = sys.argv[1]
latest = {}
for page in json.load(sys.stdin):
    for artifact in page['artifacts']:
        name = artifact['name']
        if (not name.startswith(prefix) or name == prefix + 'report'
                or artifact.get('expired')):
            continue
        rank = (artifact['created_at'], artifact['id'])
        if name not in latest or rank > latest[name][0]:
            latest[name] = (rank, artifact['id'])
if not latest:
    raise SystemExit('No lane artifacts are available for the report')
print(','.join(str(latest[name][1]) for name in sorted(latest)))
