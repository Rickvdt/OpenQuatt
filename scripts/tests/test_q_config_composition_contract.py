from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SINGLE_TARGET = (ROOT / "configs" / "heatpump_controller_q" / "single.yaml").read_text()
DUO_TARGET = (ROOT / "configs" / "heatpump_controller_q" / "duo.yaml").read_text()
SINGLE_TOPOLOGY = (ROOT / "openquatt" / "topology" / "single.yaml").read_text()
DUO_TOPOLOGY = (ROOT / "openquatt" / "topology" / "duo.yaml").read_text()
SINGLE_TOPOLOGY_PACKAGE = (ROOT / "openquatt" / "topology" / "single_package.yaml").read_text()
DUO_TOPOLOGY_PACKAGE = (ROOT / "openquatt" / "topology" / "duo_package.yaml").read_text()
Q_PROFILE = (ROOT / "openquatt" / "profiles" / "heatpump_controller_q.yaml").read_text()
NETWORK_PROFILE = (ROOT / "openquatt" / "connection" / "wifi_eth.yaml").read_text()
HIL_DUO = (ROOT / "configs" / "heatpump_controller_q" / "duo_hil.yaml").read_text()
HIL_DUO_COMPAT = (ROOT / "configs" / "heatpump_controller_q" / "duo_wifi_hil.yaml").read_text()


def yaml_scalar(text: str, key: str) -> str:
    match = re.search(
        rf'(?m)^\s*{re.escape(key)}\s*:\s*"([^"]*)"\s*(?:#.*)?$',
        text,
    )
    if not match:
        raise AssertionError(f"missing YAML scalar {key}")
    return match.group(1)


def yaml_key_pattern(key: str) -> str:
    return rf'(?m)^\s*{re.escape(key)}\s*:'


class QConfigCompositionContractTest(unittest.TestCase):
    def test_release_targets_only_pin_topology(self) -> None:
        for target, flag in (
            (SINGLE_TARGET, '-DOQ_TOPOLOGY_DUO=0'),
            (DUO_TARGET, '-DOQ_TOPOLOGY_DUO=1'),
        ):
            self.assertRegex(
                target,
                rf'(?m)^\s*oq_topology_build_flag\s*:\s*"{re.escape(flag)}"\s*$',
            )
            for key in (
                "oq_hardware_profile",
                "oq_hardware_build_flag",
                "oq_connection",
                "oq_connection_text_internal",
                "main_release_manifest_url",
                "alternate_topology",
            ):
                self.assertNotRegex(target, yaml_key_pattern(key))

        for package in (SINGLE_TOPOLOGY_PACKAGE, DUO_TOPOLOGY_PACKAGE):
            self.assertNotRegex(package, yaml_key_pattern("oq_topology_build_flag"))

    def test_hardware_and_network_have_single_owners(self) -> None:
        self.assertIn('oq_hardware_profile: "heatpump_controller_q"', Q_PROFILE)
        self.assertIn('oq_hardware_build_flag: "-DOQ_HARDWARE_HEATPUMP_CONTROLLER_Q=1"', Q_PROFILE)
        self.assertIn('oq_local_supply_temp_sensor_id: "water_supply_temp_pt1000"', Q_PROFILE)
        self.assertIn('oq_local_supply_temp_selector_id: "oq_local_supply_temp_source"', Q_PROFILE)
        self.assertIn('oq_connection: "auto"', NETWORK_PROFILE)
        self.assertIn('oq_connection_text_internal: "false"', NETWORK_PROFILE)

    # Fork: the dev channel deliberately resolves to this fork's dev-latest so the
    # built-in updater installs fork builds; main and release_manifest_url stay on
    # upstream. Only the repository owner differs - the path shape, topology
    # templating and channel routing are still asserted exactly as upstream wrote
    # them, so a genuine regression in the routing would still fail here.
    DEV_OWNER = "Rickvdt/OpenQuatt"

    def test_topology_package_owns_expanded_manifest_routing(self) -> None:
        for package, topology_config, topology, alternate in (
            (SINGLE_TOPOLOGY_PACKAGE, SINGLE_TOPOLOGY, "single", "duo"),
            (DUO_TOPOLOGY_PACKAGE, DUO_TOPOLOGY, "duo", "single"),
        ):
            self.assertEqual(yaml_scalar(topology_config, "oq_topology"), topology)
            self.assertEqual(yaml_scalar(package, "alternate_topology"), alternate)
            self.assertIn(f'!include {topology}.yaml', package)

            main_url = yaml_scalar(package, "main_release_manifest_url").replace(
                "${oq_topology}", topology
            )
            dev_url = yaml_scalar(package, "dev_release_manifest_url").replace(
                "${oq_topology}", topology
            )
            alternate_main_url = yaml_scalar(
                package, "alternate_topology_main_release_manifest_url"
            ).replace("${alternate_topology}", alternate)
            alternate_dev_url = yaml_scalar(
                package, "alternate_topology_dev_release_manifest_url"
            ).replace("${alternate_topology}", alternate)

            self.assertEqual(
                main_url,
                f"https://github.com/OpenQuatt/OpenQuatt/releases/latest/download/openquatt-heatpump-controller-q-{topology}-ota.manifest.json",
            )
            self.assertEqual(
                dev_url,
                f"https://github.com/{self.DEV_OWNER}/releases/download/dev-latest/openquatt-heatpump-controller-q-{topology}-ota.manifest.json",
            )
            self.assertEqual(
                alternate_main_url,
                f"https://github.com/OpenQuatt/OpenQuatt/releases/latest/download/openquatt-heatpump-controller-q-{alternate}-ota.manifest.json",
            )
            self.assertEqual(
                alternate_dev_url,
                f"https://github.com/{self.DEV_OWNER}/releases/download/dev-latest/openquatt-heatpump-controller-q-{alternate}-ota.manifest.json",
            )
            self.assertEqual(
                yaml_scalar(package, "release_manifest_url"),
                "${main_release_manifest_url}",
            )

    def test_hil_build_has_canonical_unified_entrypoint_and_wifi_shim(self) -> None:
        self.assertIn('openquatt_q_duo: !include duo.yaml', HIL_DUO)
        self.assertNotIn('!include duo_wifi.yaml', HIL_DUO)
        self.assertIn('!include duo_hil.yaml', HIL_DUO_COMPAT)
        self.assertNotIn('flash_write_interval: 1s', HIL_DUO_COMPAT)


if __name__ == "__main__":
    unittest.main()
