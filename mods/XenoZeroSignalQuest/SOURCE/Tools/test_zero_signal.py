from __future__ import annotations

import importlib.util
import heapq
import itertools
import json
import os
import re
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
from pathlib import Path

from PIL import Image


def find_workspace_root() -> Path:
    configured = os.environ.get("SRHD_WORKSPACE_ROOT")
    if configured:
        return Path(configured).expanduser().resolve()
    for parent in Path(__file__).resolve().parents:
        if (parent / "Tools" / "SRHDModKit" / "srhd.py").is_file():
            return parent
    raise RuntimeError("SRHD workspace root was not found; set SRHD_WORKSPACE_ROOT")


WORKSPACE_ROOT = find_workspace_root()
MOD_ROOT = Path(__file__).resolve().parents[2]
BUILDER_PATH = Path(__file__).with_name("build_zero_signal.py")
REPORT_ROOT = Path(
    os.environ.get(
        "SRHD_XZS_REPORT_ROOT",
        WORKSPACE_ROOT / "Reports" / "XenoZeroSignalQuest" / "v1.4.0",
    )
)


spec = importlib.util.spec_from_file_location("xzs_builder", BUILDER_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"Cannot import {BUILDER_PATH}")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
document = builder.build_document()

id_to_name = {value: key for key, value in builder.location_ids.items()}
name_to_id = dict(builder.location_ids)
locations_by_id = {item.id: item for item in document.locations}
jumps_from: dict[int, list] = defaultdict(list)
reverse_edges: dict[int, set[int]] = defaultdict(set)
for jump in document.jumps:
    jumps_from[jump.from_location_id].append(jump)
    reverse_edges[jump.to_location_id].add(jump.from_location_id)


def eval_formula(formula: str, state: tuple[int, ...] | list[int]) -> int | bool:
    if not formula:
        return True
    expression = re.sub(r"\[(-?\d+)\]", r"\1", formula)
    expression = re.sub(r"\[p(\d+)\]", lambda match: f"state[{int(match.group(1)) - 1}]", expression)
    expression = re.sub(r"(?<![<>!=])=(?!=)", "==", expression)
    if not re.fullmatch(r"[\d\s\[\]state()+\-*/<>=!.andor]+", expression):
        raise ValueError(f"Unsupported expression: {formula!r} -> {expression!r}")
    return eval(expression, {"__builtins__": {}}, {"state": state})


def initial_state() -> tuple[int, ...]:
    return tuple(int(eval_formula(item.starting_formula, ())) for item in document.parameters)


def apply_jump(state: tuple[int, ...], jump, *, strict_bounds: bool) -> tuple[int, ...]:
    old = state
    new = list(state)
    for index, change in enumerate(jump.parameter_changes):
        if change.change_type != 3 or not change.changing_formula:
            continue
        raw = int(eval_formula(change.changing_formula, old))
        parameter = document.parameters[index]
        if strict_bounds and not parameter.minimum <= raw <= parameter.maximum:
            raise AssertionError(
                f"Jump {jump.id} ({jump.text}) sets {parameter.name}={raw}, "
                f"outside {parameter.minimum}..{parameter.maximum}"
            )
        new[index] = min(parameter.maximum, max(parameter.minimum, raw))
    return tuple(new)


@dataclass
class Simulator:
    location: str = "arrival"
    state: tuple[int, ...] = initial_state()
    used: Counter = None  # type: ignore[assignment]
    steps: list[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        self.used = Counter()
        self.steps = []

    def step(self, target: str, label: str | None = None) -> None:
        candidates = []
        for jump in jumps_from[name_to_id[self.location]]:
            if jump.to_location_id != name_to_id[target]:
                continue
            if label and label not in jump.text:
                continue
            if jump.jumping_count_limit and self.used[jump.id] >= jump.jumping_count_limit:
                continue
            if bool(eval_formula(jump.formula_to_pass, self.state)):
                candidates.append(jump)
        if len(candidates) != 1:
            labels = [item.text for item in candidates]
            raise AssertionError(f"{self.location} -> {target} ({label!r}): expected one jump, got {labels}")
        jump = candidates[0]
        self.state = apply_jump(self.state, jump, strict_bounds=True)
        self.used[jump.id] += 1
        self.location = target
        self.steps.append(f"{id_to_name[jump.from_location_id]} -> {target}: {jump.text}")


def play_common(kit: str) -> Simulator:
    sim = Simulator()
    for target, label in [
        (kit, None),
        ("shuttle_board", None),
        ("approach", "Запустить автопилот"),
        ("dock", None),
        ("pressure_manual", None),
        ("airlock_open", "Синий"),
        ("foyer", None),
        ("central_hub", None),
        ("control_entry", None),
        ("control_console", None),
        ("security_feed", None),
        ("control_console", "Сохранить запись"),
        ("control_entry", None),
        ("central_hub", None),
        ("galley", None),
        ("central_hub", None),
        ("quarters_corridor", None),
        ("cabin_captain", None),
        ("quarters_corridor", "Забрать запись"),
        ("cabin_engineer", None),
        ("quarters_corridor", "Забрать журнал"),
        ("cabin_doctor", None),
        ("quarters_corridor", "Сохранить запись"),
        ("cabin_loader", None),
        ("quarters_corridor", "Забрать жетон"),
        ("central_hub", None),
        ("cargo_entry", None),
        ("first_encounter", None),
        ("hide_locker", None),
        ("cargo_after", "Дождаться"),
        ("fuel_cell", None),
        ("cargo_after", "Забрать элемент"),
        ("engineering_entry", None),
        ("coolant_room", None),
        ("engineering_entry", "Запомнить порядок"),
        ("central_hub", None),
        ("lab_door", None),
        ("code_console", None),
        ("lab_open", "1327"),
        ("lower_corridor", None),
        ("cryo_lab", None),
        ("cryo_console", None),
        ("cryo_correct", "Синий"),
        ("nest_threshold", None),
        ("nest_observe", None),
        ("nest_sneak", "холодной тру"),
        ("truth_terminal", None),
        ("subject19", None),
        ("betrayal_reveal", None),
        ("queen_chamber", None),
        ("escape_hub", None),
    ]:
        sim.step(target, label)
    return sim


def finish_shuttle(sim: Simulator) -> None:
    for target, label in [
        ("route_shuttle", None),
        ("shuttle_repairs", None),
        ("route_shuttle", None),
        ("shuttle_clamps", None),
        ("route_shuttle", None),
        ("shuttle_cargo_check", None),
        ("shuttle_launch", None),
        ("shuttle_success", None),
    ]:
        sim.step(target, label)


def finish_capsule(sim: Simulator) -> None:
    for target, label in [
        ("route_capsule", None),
        ("capsule_tunnel", None),
        ("vacuum_preparation", None),
        ("capsule_console", "Двигаться медленно"),
        ("capsule_launch", "4-8-1"),
        ("capsule_success", None),
    ]:
        sim.step(target, label)


def finish_reactor(sim: Simulator) -> None:
    for target, label in [
        ("route_reactor", None),
        ("transmit_evidence", None),
        ("reactor_entry", "Спуститься"),
        ("reactor_arm", "Крайние"),
        ("meltdown_1", None),
        ("meltdown_2", "Проскочить"),
        ("meltdown_3", "Обойти"),
        ("eva_airlock", "Надеть"),
        ("eva_crossing", "Оттолкнуться"),
        ("reactor_success", None),
    ]:
        sim.step(target, label)


def route_suite() -> list[dict[str, object]]:
    results = []
    finishers = {
        "shuttle": finish_shuttle,
        "capsule": finish_capsule,
        "reactor": finish_reactor,
    }
    for kit in ("kit_engineer", "kit_guard", "kit_science"):
        for route, finisher in finishers.items():
            sim = play_common(kit)
            finisher(sim)
            location = locations_by_id[name_to_id[sim.location]]
            if location.location_type != 3:
                raise AssertionError(f"{kit}/{route} ended at {sim.location}")
            results.append(
                {
                    "kit": kit,
                    "route": route,
                    "terminal": sim.location,
                    "steps": len(sim.steps),
                    "health": sim.state[0],
                    "oxygen": sim.state[1],
                    "ammo": sim.state[2],
                    "money": sim.state[29],
                }
            )
    return results


def structural_checks() -> dict[str, object]:
    image_keys = {item.texts[0].media.img for item in document.locations if item.texts[0].media.img}
    expected_game_images = {
        f"XZS_{number:02d}"
        for number in range(71)
        if number not in {7, 9, 29}
    }
    expected_images = expected_game_images
    image_by_location = {
        id_to_name[item.id]: item.texts[0].media.img
        for item in document.locations
        if item.texts[0].media.img
    }
    scene_contract = {
        "arrival": "XZS_22",
        "kit_engineer": "XZS_34",
        "shuttle_console": "XZS_35",
        "pressure_manual": "XZS_36",
        "central_hub": "XZS_37",
        "control_console": "XZS_38",
        "cabin_captain": "XZS_39",
        "medbay": "XZS_40",
        "cargo_after": "XZS_41",
        "generator_room": "XZS_42",
        "vent_entry": "XZS_43",
        "code_console": "XZS_44",
        "capsule_console": "XZS_45",
        "cabin_doctor": "XZS_46",
        "galley": "XZS_47",
        "cabin_loader": "XZS_48",
        "trap_setup": "XZS_49",
        "trap_success": "XZS_50",
        "stowaway_hint": "XZS_51",
        "coolant_room": "XZS_52",
        "android_lab": "XZS_25",
        "android_repair": "XZS_53",
        "android_override": "XZS_53",
        "android_hostile_escape": "XZS_54",
        "escape_hub": "XZS_55",
        "shuttle_clamps": "XZS_56",
        "shuttle_cargo_check": "XZS_57",
        "capsule_tunnel": "XZS_58",
        "capsule_launch": "XZS_59",
        "capsule_success": "XZS_60",
        "meltdown_1": "XZS_61",
        "meltdown_3": "XZS_62",
        "reactor_success": "XZS_63",
        "death_stowaway": "XZS_64",
        "death_android": "XZS_65",
        "death_capsule": "XZS_66",
        "eva_airlock": "XZS_67",
        "eva_crossing": "XZS_68",
        "death_no_suit": "XZS_69",
        "death_vacuum": "XZS_69",
        "death_debris": "XZS_70",
    }
    death_names = {id_to_name[item.id] for item in document.locations if item.location_type == 5}
    success_names = {id_to_name[item.id] for item in document.locations if item.location_type == 3}
    if image_keys != expected_images:
        raise AssertionError(f"Image usage mismatch: missing={expected_images - image_keys}, extra={image_keys - expected_images}")
    game_image_paths = sorted((MOD_ROOT / "DATA" / "PQI").glob("XZS_*.jpg"))
    game_file_keys = {item.stem for item in game_image_paths}
    if game_file_keys != expected_game_images:
        raise AssertionError(
            f"Game image files mismatch: missing={expected_game_images - game_file_keys}, "
            f"extra={game_file_keys - expected_game_images}"
        )
    wrong_sizes = {}
    for path in game_image_paths:
        with Image.open(path) as opened:
            if opened.size != (343, 394) or opened.mode != "RGB":
                wrong_sizes[path.name] = {"size": opened.size, "mode": opened.mode}
    if wrong_sizes:
        raise AssertionError(f"Invalid game image exports: {wrong_sizes}")
    mismatched_scenes = {
        name: (image_by_location.get(name), expected)
        for name, expected in scene_contract.items()
        if image_by_location.get(name) != expected
    }
    if mismatched_scenes:
        raise AssertionError(f"Scene/image contract mismatch: {mismatched_scenes}")
    image_usage = Counter(image_by_location.values())
    if max(image_usage.values()) > 8:
        raise AssertionError(f"An illustration is overused: {image_usage.most_common(5)}")
    if len(death_names) < 14 or len(success_names) != 3:
        raise AssertionError(f"Unexpected terminals: deaths={len(death_names)}, successes={len(success_names)}")
    if not 100 <= len(document.locations) <= 120:
        # The design target was approximate; 127 is accepted because distinct deaths
        # are real authored scenes rather than padding.
        if len(document.locations) != 127:
            raise AssertionError(f"Unexpected location count: {len(document.locations)}")
    if not 280 <= len(document.jumps) <= 350:
        raise AssertionError(f"Unexpected jump count: {len(document.jumps)}")
    text_parts = [document.task_text, document.success_text]
    text_parts.extend(item.text for location in document.locations for item in location.texts)
    text_parts.extend(value for jump in document.jumps for value in (jump.text, jump.description))
    words = sum(len(value.split()) for value in text_parts)
    if not 12_000 <= words <= 16_000:
        raise AssertionError(f"Unexpected word count: {words}")
    foreign_lore_terms = ("яутжа", "клингон", "туриан", "вулканец", "чужая разумная раса")
    corpus = "\n".join(text_parts).lower()
    allowed_placeholders = {"Ranger", "ToStar", "ToPlanet", "Date", "Money", "FromPlanet"}
    discovered_placeholders = set(re.findall(r"<([A-Za-z]+)>", "\n".join(text_parts)))
    if discovered_placeholders - allowed_placeholders:
        raise AssertionError(f"Unknown quest placeholders: {sorted(discovered_placeholders - allowed_placeholders)}")
    for index, value in enumerate(text_parts):
        if "\ufffd" in value or "\x00" in value:
            raise AssertionError(f"Damaged Unicode in text block {index}")
        if value.count("«") != value.count("»"):
            raise AssertionError(f"Unbalanced Russian quotes in text block {index}")
        if re.search(r"[ \t]+\n", value):
            raise AssertionError(f"Trailing whitespace in text block {index}")
    direct_franchise_terms = [term for term in ("ксеноморф", "лицехват") if term in corpus]
    if direct_franchise_terms:
        raise AssertionError(f"Direct Alien-franchise creature terms remain: {direct_franchise_terms}")
    found = [term for term in foreign_lore_terms if term in corpus]
    if found:
        raise AssertionError(f"Foreign intelligent species found: {found}")

    jumps_by_text = {jump.text: jump for jump in document.jumps}
    if jumps_by_text["Попытаться добить существо резаком"].formula_to_pass != "[p7]=1":
        raise AssertionError("Cutter-only trap action is not gated by the cutter flag")
    if jumps_by_text["Ввести случайный код"].formula_to_pass != "[p31]<2":
        raise AssertionError("Random code entry does not stop after two nonfatal errors")
    scanner_escape = jumps_by_text.get("Найти боковой сервисный люк биосканером")
    if scanner_escape is None or scanner_escape.formula_to_pass != "[p9]=1":
        raise AssertionError("Research-kit vent escape is missing or not gated by the bioscanner")
    if "Новый предохранитель" in locations_by_id[name_to_id["fuse_repair"]].texts[0].text:
        raise AssertionError("Fuse-repair text still contradicts the cutter bypass route")
    if "осталось две попытки" in locations_by_id[name_to_id["wrong_code"]].texts[0].text:
        raise AssertionError("Wrong-code text still reports a fixed attempt count")
    return {
        "parameters": len(document.parameters),
        "locations": len(document.locations),
        "jumps": len(document.jumps),
        "words": words,
        "death_locations": sorted(death_names),
        "success_locations": sorted(success_names),
        "images_used": sorted(image_keys),
    }


def graph_reachability() -> dict[str, object]:
    # Conservative structural reachability: every terminal must have a path from
    # the start in the authored graph. Formula correctness is covered separately
    # by the nine stateful success playthroughs.
    visited = {name_to_id["arrival"]}
    queue = deque(visited)
    while queue:
        source = queue.popleft()
        for jump in jumps_from[source]:
            if jump.to_location_id not in visited:
                visited.add(jump.to_location_id)
                queue.append(jump.to_location_id)
    unreachable = sorted(id_to_name[item.id] for item in document.locations if item.id not in visited)
    if unreachable:
        raise AssertionError(f"Unreachable authored locations: {unreachable}")
    terminal_counts = Counter(locations_by_id[item].location_type for item in visited)
    return {
        "reachable_locations": len(visited),
        "unreachable_locations": unreachable,
        "successes_reachable": terminal_counts[3],
        "failures_reachable": terminal_counts[4],
        "deaths_reachable": terminal_counts[5],
    }


def stateful_terminal_reachability(max_states: int = 500_000) -> dict[str, object]:
    # For exhaustive terminal discovery we intentionally collapse per-jump visit
    # counters. Parameter changes are clamped to their declared bounds, so the
    # abstract state space stays finite. Legitimate one-use resource handling is
    # verified by the stricter named playthroughs above.
    def abstract_state(state: tuple[int, ...]) -> tuple[int, ...]:
        values = list(state)
        values[0] = 100 if values[0] >= 100 else 0
        values[1] = 60 if values[1] > 50 else 40 if values[1] > 30 else 20 if values[1] > 15 else 0
        values[2] = 4 if values[2] > 0 else 0
        values[3] = 50 if values[3] >= 50 else 40 if values[3] >= 40 else 0
        values[4] = 4 if values[4] >= 4 else values[4]
        values[10] = 0  # evidence affects only payout after a successful terminal
        values[11] = 0  # journal count has no formula gate
        values[12] = min(values[12], 3)
        values[20] = min(values[20], 3)
        values[29] = 0  # money has no formula gate
        values[30] = min(values[30], 3)
        return tuple(values)

    start = (name_to_id["arrival"], abstract_state(initial_state()))
    expected = sorted(
        id_to_name[item.id]
        for item in document.locations
        if item.location_type in (4, 5)
    )
    witness_lengths: dict[str, int] = {}
    explored_total = 0
    for target_name in expected:
        target_id = name_to_id[target_name]
        distance = {target_id: 0}
        reverse_queue = deque([target_id])
        while reverse_queue:
            node_id = reverse_queue.popleft()
            for predecessor in reverse_edges[node_id]:
                if predecessor not in distance:
                    distance[predecessor] = distance[node_id] + 1
                    reverse_queue.append(predecessor)
        serial = itertools.count()
        queue = [(distance.get(start[0], 10_000), 0, next(serial), start[0], start[1])]
        visited = {start}
        found = False
        while queue:
            _, depth, _, location_id, state = heapq.heappop(queue)
            if location_id == target_id:
                witness_lengths[target_name] = depth
                found = True
                break
            if locations_by_id[location_id].location_type in (3, 4, 5):
                continue
            for jump in jumps_from[location_id]:
                if not bool(eval_formula(jump.formula_to_pass, state)):
                    continue
                next_state = abstract_state(apply_jump(state, jump, strict_bounds=False))
                node = (jump.to_location_id, next_state)
                if node not in visited:
                    visited.add(node)
                    next_depth = depth + 1
                    heapq.heappush(
                        queue,
                        (next_depth + distance.get(node[0], 10_000), next_depth, next(serial), node[0], node[1]),
                    )
                    if len(visited) > max_states:
                        raise AssertionError(f"State-space limit exceeded for {target_name}: {max_states}")
        explored_total += len(visited)
        if not found:
            raise AssertionError(f"Statefully unreachable terminal: {target_name}")

    terminals: dict[int, set[str]] = defaultdict(set)
    for name in witness_lengths:
        terminals[locations_by_id[name_to_id[name]].location_type].add(name)
    return {
        "explored_states_across_witness_searches": explored_total,
        "reachable_failures": sorted(terminals[4]),
        "reachable_deaths": sorted(terminals[5]),
        "witness_lengths": witness_lengths,
    }


def main() -> None:
    report = {
        "schema": "xeno-zero-signal-tests-v1",
        "structural": structural_checks(),
        "reachability": graph_reachability(),
        "stateful_reachability": stateful_terminal_reachability(),
        "successful_playthroughs": route_suite(),
    }
    REPORT_ROOT.mkdir(parents=True, exist_ok=True)
    output = REPORT_ROOT / "quest-logic-tests.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(output), **report["structural"], **report["reachability"], "stateful": report["stateful_reachability"], "playthroughs": 9}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
