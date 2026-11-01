"""Capture repeatable visual receipts for the local capsule editor.

This opens the already-running loopback UI, selects one fixed machine, verifies
real preview images, opens the unit picker, and invokes client-overlay compile.
It never clicks Save, Import, Retire, Restore, or Initialize. Apply success and
running-client states are exercised only through Playwright-intercepted JSON;
no real apply request reaches the local server.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from playwright.sync_api import sync_playwright


DEFAULT_CHROME = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")


def simulated_apply_page(
    browser,
    *,
    url,
    machine_id,
    output,
    filename,
    result,
    expected_title,
    console_errors,
    failed_requests,
):
    page = browser.new_page(viewport={"width": 1440, "height": 1000})
    page.on(
        "console",
        lambda message: console_errors.append(message.text)
        if message.type == "error"
        else None,
    )
    page.on(
        "requestfailed",
        lambda request: failed_requests.append(
            {"url": request.url, "reason": request.failure}
        ),
    )
    apply_intercepts = []

    def override_bootstrap(route):
        response = route.fetch()
        data = response.json()
        data["capabilities"]["apply"] = True
        route.fulfill(
            status=200,
            content_type="application/json; charset=utf-8",
            body=json.dumps(data, ensure_ascii=False),
        )

    def override_apply(route):
        apply_intercepts.append(route.request.post_data_json)
        route.fulfill(
            status=200,
            content_type="application/json; charset=utf-8",
            body=json.dumps(result, ensure_ascii=False),
        )

    page.route("**/api/bootstrap", override_bootstrap)
    page.route("**/api/apply", override_apply)
    page.on("dialog", lambda dialog: dialog.accept())
    page.goto(url, wait_until="networkidle", timeout=120_000)
    page.locator(f'#machine-list [data-machine-id="{machine_id}"]').click()
    page.locator("#editor:not(.hidden)").wait_for(state="visible")
    button = page.locator("#apply-button")
    if not button.is_enabled():
        raise RuntimeError("Simulated configured target did not enable Apply")
    button.click()
    page.locator("#result-dialog[open]").wait_for(state="visible", timeout=120_000)
    title = page.locator("#result-title").inner_text()
    content = page.locator("#result-content").inner_text()
    if title != expected_title:
        raise RuntimeError(f"Simulated Apply reported an unexpected state: {title}")
    if len(apply_intercepts) != 1:
        raise RuntimeError("Simulated Apply was not intercepted exactly once")
    page.screenshot(path=str(output / filename))
    receipt = {
        "simulated": True,
        "button_enabled": True,
        "result_title": title,
        "result_content": content,
        "intercepted_request": apply_intercepts[0],
        "real_apply_invoked": False,
    }
    page.close()
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine-id", type=int, default=7901)
    parser.add_argument("--chrome", type=Path, default=DEFAULT_CHROME)
    args = parser.parse_args()

    if not args.url.startswith("http://127.0.0.1:"):
        parser.error("--url must be a 127.0.0.1 loopback URL")
    if not args.chrome.is_file():
        parser.error(f"Chrome executable is missing: {args.chrome}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    console_errors = []
    failed_requests = []

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(
            executable_path=str(args.chrome),
            headless=True,
            args=["--disable-gpu"],
        )
        page = browser.new_page(viewport={"width": 1440, "height": 1000})
        page.on(
            "console",
            lambda message: console_errors.append(message.text)
            if message.type == "error"
            else None,
        )
        page.on(
            "requestfailed",
            lambda request: failed_requests.append(
                {"url": request.url, "reason": request.failure}
            ),
        )
        page.goto(args.url, wait_until="networkidle", timeout=120_000)
        page.locator("#machine-list").wait_for(state="visible")

        machine_button = page.locator(
            f'#machine-list [data-machine-id="{args.machine_id}"]'
        )
        if machine_button.count() != 1:
            raise RuntimeError(f"Machine {args.machine_id} was not listed exactly once")
        machine_button.click()
        page.locator("#editor:not(.hidden)").wait_for(state="visible")
        page.locator("#fixed-slots .fixed-row").first.wait_for(state="visible")
        source_label = page.locator("#field-owner-label").inner_text()
        policy_label = page.locator("#field-policy-label").inner_text()
        if page.locator("#field-owner, #field-policy").count() != 0:
            raise RuntimeError("Engineering owner/policy inputs remain editable")
        percentages = page.locator("#fixed-slots .slot-percent").all_inner_texts()
        if len(percentages) != 6 or any(not value.endswith("%") for value in percentages):
            raise RuntimeError("Fixed weights do not show six percentage values")
        advanced_count = page.locator("#fixed-slots details.entry-advanced").count()
        if advanced_count != 6:
            raise RuntimeError("Native slot metadata is not contained in advanced details")
        apply_button = page.locator("#apply-button")
        apply_enabled = apply_button.is_enabled()
        if apply_enabled:
            raise RuntimeError("Apply unexpectedly enabled without a configured target")
        page.screenshot(path=str(output / "01-machine-selected.png"))

        badge = page.locator("#editor-location").inner_text()
        management_position = int(page.locator("#field-position").input_value())
        thumbnails = page.locator("#fixed-slots img.unit-thumb")
        thumbnail_count = thumbnails.count()
        thumbnail_receipts = []
        for index in range(thumbnail_count):
            image = thumbnails.nth(index)
            image.wait_for(state="visible")
            receipt = image.evaluate(
                """image => ({
                    src: image.getAttribute("src"),
                    complete: image.complete,
                    naturalWidth: image.naturalWidth,
                    naturalHeight: image.naturalHeight
                })"""
            )
            thumbnail_receipts.append(receipt)
        if thumbnail_count != 6 or any(
            not item["complete"]
            or item["naturalWidth"] <= 0
            or not item["src"].startswith("/assets/unit/")
            for item in thumbnail_receipts
        ):
            raise RuntimeError("Selected machine did not render six real unit previews")

        page.locator("#fixed-slots [data-pick-unit]").first.click()
        page.locator("#unit-dialog[open]").wait_for(state="visible")
        picker_images = page.locator("#unit-grid img.unit-thumb")
        picker_images.first.wait_for(state="visible")
        picker_preview = picker_images.first.evaluate(
            """image => ({
                src: image.getAttribute("src"),
                complete: image.complete,
                naturalWidth: image.naturalWidth,
                naturalHeight: image.naturalHeight
            })"""
        )
        if (
            not picker_preview["complete"]
            or picker_preview["naturalWidth"] <= 0
            or not picker_preview["src"].startswith("/assets/unit/")
        ):
            raise RuntimeError("Unit picker did not render a real preview image")
        page.screenshot(path=str(output / "02-unit-picker.png"))
        page.locator(
            '#unit-dialog [data-close-dialog="unit-dialog"]'
        ).first.click()

        compile_button = page.locator("#compile-button")
        compile_enabled = compile_button.is_enabled()
        if not compile_enabled:
            raise RuntimeError("Compile button is disabled")
        compile_button.click()
        page.locator("#result-dialog[open]").wait_for(
            state="visible", timeout=300_000
        )
        result_title = page.locator("#result-title").inner_text()
        result_content = page.locator("#result-content").inner_text()
        if result_title != "客户端 Overlay 已编译":
            raise RuntimeError(f"Compile UI reported failure: {result_title}")
        if "不会安装到游戏客户端" not in result_content:
            raise RuntimeError("Compile result did not preserve the install boundary")
        page.screenshot(path=str(output / "03-compile-result.png"))

        mock_runtime = str(
            output / "mock-runtime" / "client-v2"
        )
        mock_placements = str(
            output / "mock-runtime" / "client-v2" / "placements.json"
        )
        simulated_success = simulated_apply_page(
            browser,
            url=args.url,
            machine_id=args.machine_id,
            output=output,
            filename="04-simulated-apply-success.png",
            result={
                "applied": True,
                "message": "模拟回调已确认预配置目标导入完成。",
                "catalog_revision": "e0232501b6e4",
                "runtime_output": mock_runtime,
                "placements_path": mock_placements,
                "selected_placement": {
                    "id": args.machine_id,
                    "position": 79,
                    "page": 10,
                    "page_slot": 7,
                },
            },
            expected_title="已导入到游戏",
            console_errors=console_errors,
            failed_requests=failed_requests,
        )
        simulated_running = simulated_apply_page(
            browser,
            url=args.url,
            machine_id=args.machine_id,
            output=output,
            filename="05-simulated-apply-running.png",
            result={
                "applied": False,
                "message": "Target client is running; close it first",
                "catalog_revision": "e0232501b6e4",
                "runtime_output": mock_runtime,
                "placements_path": mock_placements,
            },
            expected_title="请先关闭目标客户端",
            console_errors=console_errors,
            failed_requests=failed_requests,
        )

        receipt = {
            "schema_version": 1,
            "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "url": args.url,
            "title": page.title(),
            "revision_text": page.locator("#revision").inner_text(),
            "machine": {
                "id": args.machine_id,
                "management_position": management_position,
                "editor_location_badge": badge,
                "fixed_slot_count": page.locator(
                    "#fixed-slots .fixed-row"
                ).count(),
                "thumbnail_count": thumbnail_count,
                "thumbnails": thumbnail_receipts,
                "source_label": source_label,
                "policy_label": policy_label,
                "percentages": percentages,
                "advanced_detail_count": advanced_count,
            },
            "unit_picker": {
                "result_count_text": page.locator(
                    "#unit-result-count"
                ).inner_text(),
                "first_preview": picker_preview,
            },
            "compile": {
                "button_enabled": compile_enabled,
                "result_title": result_title,
                "result_content": result_content,
            },
            "apply": {
                "button_enabled": apply_enabled,
                "button_title": apply_button.get_attribute("title"),
                "invoked": False,
                "simulated_success": simulated_success,
                "simulated_running_client": simulated_running,
            },
            "console_errors": console_errors,
            "failed_requests": failed_requests,
            "screenshots": [
                "01-machine-selected.png",
                "02-unit-picker.png",
                "03-compile-result.png",
                "04-simulated-apply-success.png",
                "05-simulated-apply-running.png",
            ],
        }
        (output / "visual-receipt.json").write_text(
            json.dumps(receipt, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        browser.close()
    print(json.dumps(receipt, ensure_ascii=False))


if __name__ == "__main__":
    main()
