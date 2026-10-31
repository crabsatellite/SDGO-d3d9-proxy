"""Portable profile and package contract, with no account, tunnel, or game access."""
import configparser
import json
import copy
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from make_frp_profile import configurations, export_profile
from package_frp import symbol_fragment, validate_markdown_links


class DistributionChecks(unittest.TestCase):
    def setUp(self):
        self.profile = dict(schema_version=2, udp_bind_ip="192.0.2.44",
                            login=dict(logical_ip="192.0.2.45", logical_port=16543,
                                       external_host="other-owner.example.invalid", external_port=41357),
                            udp=[dict(local_port=23100+i, external_host="other-owner.example.invalid",
                                      external_port=p) for i, p in enumerate((49731, 38163, 54329, 21079))],
                            local_path=r"Q:\private-host", sakura_token="SECRET_TOKEN",
                            database=dict(password="SECRET_DATABASE"))

    def test_player_export_has_only_public_connection_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive=export_profile(self.profile,Path(temporary)/"different layout"/"export")
            with zipfile.ZipFile(archive) as z:
                self.assertEqual(set(z.namelist()),{"README.txt","sdgo/mods/net_route/config.ini"})
                payload=b"".join(z.read(n) for n in z.namelist())
                for private in (b"SECRET",b"private-host",b"Agent",b"Start-Process",b"sdgo.ini"):
                    self.assertNotIn(private,payload)
                config=configparser.ConfigParser();config.read_string(z.read('sdgo/mods/net_route/config.ini').decode())
                self.assertEqual(config.sections(),["settings"])
                self.assertEqual(config['settings']['LoginIPv4'],"192.0.2.45")
                self.assertEqual(config['settings']['LoginPort'],"16543")
                self.assertEqual(config['settings']['RouteCount'],"5")
                self.assertIn('49731',config['settings']['Route2'])
            with self.assertRaises(FileExistsError):export_profile(self.profile,archive.parent)

    def test_arbitrary_owner_endpoints_and_legacy_schema(self):
        first=configurations(self.profile)
        other=copy.deepcopy(self.profile);other['schema_version']=1
        other['login'].update(logical_ip='198.51.100.17',logical_port=17453,external_port=37619)
        second=configurations(other)
        self.assertNotEqual(first[0],second[0])
        self.assertIn('FirstPort=23100\nPortCount=4',first[1])
        self.assertNotIn('[mod]',first[0])
        self.assertEqual(self.profile['login']['logical_port'],16543)

    def test_invalid_pool_or_reverse_endpoint_is_rejected_before_export(self):
        for mutate in (lambda p:p['udp'][1].update(local_port=23112),
                       lambda p:p['udp'][1].update(external_port=49731),
                       lambda p:p['login'].update(external_port=True),
                       lambda p:p['login'].update(external_host='https://host:123/token')):
            profile=copy.deepcopy(self.profile);mutate(profile)
            with tempfile.TemporaryDirectory() as temporary:
                target=Path(temporary)/'export'
                with self.assertRaises(ValueError):export_profile(profile,target)
                self.assertFalse(target.exists())

    def test_logical_ip_types_and_excess_routes_are_rejected(self):
        for value in (True, 1234, None):
            profile=copy.deepcopy(self.profile);profile['udp_bind_ip']=value
            with self.assertRaises(ValueError):configurations(profile)
        profile=copy.deepcopy(self.profile)
        profile['login']['aliases']=[dict(logical_ip='198.51.100.7',logical_port=20000+i) for i in range(28)]
        with self.assertRaises(ValueError):configurations(profile)

    def test_client_symbol_payload_does_not_contain_ai_symbols(self):
        repo=Path(__file__).resolve().parents[1]
        fragment=symbol_fragment(repo/'symbols/GOnline-54BE1568-00C97000.ini',('net.login_argv_call',))
        config=configparser.ConfigParser();config.read_string(fragment.decode())
        self.assertEqual(config.sections(),['host','net.login_argv_call'])

    def test_packaged_tutorial_links_must_resolve(self):
        valid={'README.md':b'[help](docs/help.md#setup)', 'docs/help.md':b'[home](../README.md)'}
        validate_markdown_links(valid)
        with self.assertRaises(ValueError):validate_markdown_links({'README.md':b'[missing](docs/help.md)'})

    def test_manual_templates_match_the_four_slot_owner_example(self):
        repo=Path(__file__).resolve().parents[1]
        profile=json.loads((repo/'docs/examples/frp-profile.example.json').read_text())
        client,agent,_=configurations(profile)
        for mod,generated in (('net_route',client),('agent_udp_pool',agent)):
            template=configparser.ConfigParser();template.read(repo/f'mods/{mod}/config.example.ini')
            compiled=configparser.ConfigParser();compiled.read_string(generated)
            self.assertEqual(dict(template['settings']),dict(compiled['settings']))


if __name__=='__main__':unittest.main()
