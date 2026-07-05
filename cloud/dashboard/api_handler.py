#!/usr/bin/env python3
"""
api_handler.py — Lambda Function URL handler for SDV telemetry dashboard.

Returns an HTML page showing:
  - Last 20 telemetry records from DynamoDB sdv_telemetry
  - Last 10 anomaly alerts from DynamoDB sdv_anomalies
  - Auto-refreshes every 5 seconds

Deploy:
  zip dashboard.zip api_handler.py
  aws lambda create-function \
    --function-name sdv-dashboard-api \
    --runtime python3.12 \
    --handler api_handler.lambda_handler \
    --role <lambda-execution-role-arn> \
    --zip-file fileb://dashboard.zip \
    --environment Variables='{
        "TABLE_TELEMETRY":"sdv_telemetry",
        "TABLE_ANOMALIES":"sdv_anomalies",
        "TARGET_UIN":"YOUR_VIN_PLACEHOLDER"
    }'

Then: Lambda console → Configuration → Function URL → Auth type: NONE → Create
Open the Function URL in your browser — live dashboard loads.

IAM permissions needed on the Lambda execution role:
  dynamodb:Query on arn:aws:dynamodb:<region>:<account>:table/sdv_telemetry
  dynamodb:Query on arn:aws:dynamodb:<region>:<account>:table/sdv_anomalies
"""

import boto3
import json
import os
from boto3.dynamodb.conditions import Key
from decimal import Decimal

TABLE_TELEMETRY = os.environ.get("TABLE_TELEMETRY", "sdv_telemetry")
TABLE_ANOMALIES = os.environ.get("TABLE_ANOMALIES", "sdv_anomalies")
TARGET_UIN      = os.environ.get("TARGET_UIN",      "YOUR_VIN_PLACEHOLDER")

dynamo = boto3.resource("dynamodb")


class DecimalEncoder(json.JSONEncoder):
    """DynamoDB returns Decimals — convert to float for JSON serialisation."""
    def default(self, obj):
        if isinstance(obj, Decimal):
            return float(obj)
        return super().default(obj)


def query_latest(table_name: str, uin: str, limit: int = 20) -> list:
    """
    Query DynamoDB for the most recent `limit` rows for a given UIN.
    Uses ScanIndexForward=False to get newest first (DynamoDB sorts ascending by SK).
    """
    table = dynamo.Table(table_name)
    try:
        resp = table.query(
            KeyConditionExpression=Key("uin").eq(uin),
            ScanIndexForward=False,   # descending by sort key (timestamp) → newest first
            Limit=limit,
        )
        return resp.get("Items", [])
    except Exception as e:
        return [{"error": str(e)}]


def format_signals(item: dict) -> str:
    """Render signal list from a telemetry DynamoDB item as HTML table rows."""
    signals = item.get("signals", [])
    if not signals:
        return "<tr><td colspan='3' style='color:#666'>no signals</td></tr>"
    rows = ""
    for s in signals:
        name  = s.get("name",  "?")
        value = s.get("value", "?")
        unit  = s.get("unit",  "")
        if isinstance(value, (int, float, Decimal)):
            value = f"{float(value):.2f}"
        rows += f"<tr><td>{name}</td><td>{value}</td><td>{unit}</td></tr>"
    return rows


def build_html(telemetry: list, anomalies: list, uin: str) -> str:
    """Build the full HTML dashboard page."""

    # ── Telemetry table rows ─────────────────────────────────────────────────
    tele_rows = ""
    if not telemetry:
        tele_rows = "<tr><td colspan='4' style='text-align:center;color:#666'>No data yet — start the gateway on Pi.</td></tr>"
    else:
        for item in telemetry:
            ts       = item.get("timestamp", item.get("arrival_ts", "—"))
            seq      = item.get("sequence_num", "—")
            sig_html = format_signals(item)
            # Each telemetry item may have multiple signals — expand into sub-rows
            tele_rows += f"""
            <tr class="tele-header">
                <td rowspan="1">{ts}</td>
                <td rowspan="1">seq {seq}</td>
                <td colspan="2">
                    <table class="inner">
                        <tr><th>Signal</th><th>Value</th><th>Unit</th></tr>
                        {sig_html}
                    </table>
                </td>
            </tr>"""

    # ── Anomaly table rows ───────────────────────────────────────────────────
    anomaly_rows = ""
    if not anomalies:
        anomaly_rows = "<tr><td colspan='6' style='text-align:center;color:#666'>No anomalies detected yet.</td></tr>"
    else:
        for item in anomalies:
            ts       = item.get("timestamp", "—")
            signal   = item.get("signal_name", "—")
            value    = item.get("value",  "—")
            unit     = item.get("unit",   "")
            score    = item.get("anomaly_score", item.get("anomaly_prob", "—"))
            severity = item.get("severity", "WARNING")
            if isinstance(score, (int, float, Decimal)):
                score = f"{float(score):.4f}"
            if isinstance(value, (int, float, Decimal)):
                value = f"{float(value):.2f}"
            sev_class = "critical" if severity == "CRITICAL" else "warning"
            anomaly_rows += f"""
            <tr>
                <td>{ts}</td>
                <td>{signal}</td>
                <td>{value} {unit}</td>
                <td>{score}</td>
                <td class="{sev_class}">{severity}</td>
            </tr>"""

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="5">
  <title>SDV Edge Gateway — Live Dashboard</title>
  <style>
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{
      font-family: 'Segoe UI', system-ui, sans-serif;
      background: #0d1117;
      color: #e6edf3;
      padding: 24px;
    }}
    header {{
      display: flex;
      align-items: center;
      gap: 12px;
      margin-bottom: 24px;
      border-bottom: 1px solid #21262d;
      padding-bottom: 16px;
    }}
    header h1 {{ font-size: 20px; font-weight: 600; }}
    .badge {{
      background: #238636;
      color: #fff;
      font-size: 11px;
      padding: 2px 8px;
      border-radius: 12px;
      font-weight: 600;
    }}
    .uin-label {{
      font-size: 13px;
      color: #8b949e;
      margin-left: auto;
    }}
    .refresh-note {{
      font-size: 12px;
      color: #484f58;
      margin-left: 8px;
    }}
    section {{ margin-bottom: 32px; }}
    section h2 {{
      font-size: 16px;
      font-weight: 600;
      margin-bottom: 12px;
      color: #58a6ff;
    }}
    table {{
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
      background: #161b22;
      border: 1px solid #21262d;
      border-radius: 8px;
      overflow: hidden;
    }}
    th {{
      background: #21262d;
      color: #8b949e;
      font-weight: 600;
      padding: 10px 14px;
      text-align: left;
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.4px;
    }}
    td {{
      padding: 10px 14px;
      border-top: 1px solid #21262d;
      vertical-align: top;
    }}
    tr:hover td {{ background: #1c2128; }}
    table.inner {{
      background: transparent;
      border: none;
      font-size: 12px;
    }}
    table.inner th {{ background: transparent; padding: 4px 8px; }}
    table.inner td {{ padding: 4px 8px; border-top: 1px solid #21262d44; }}
    .critical {{
      color: #f85149;
      font-weight: 700;
    }}
    .warning {{
      color: #e3b341;
      font-weight: 600;
    }}
    footer {{
      margin-top: 40px;
      font-size: 11px;
      color: #484f58;
      text-align: center;
    }}
  </style>
</head>
<body>
  <header>
    <div>
      <h1>SDV Edge Gateway</h1>
    </div>
    <span class="badge">LIVE</span>
    <span class="uin-label">UIN: {uin}</span>
    <span class="refresh-note">auto-refresh every 5s</span>
  </header>

  <section>
    <h2>Telemetry — Last 20 Records</h2>
    <table>
      <thead>
        <tr>
          <th>Timestamp</th>
          <th>Seq</th>
          <th colspan="2">Signals</th>
        </tr>
      </thead>
      <tbody>
        {tele_rows}
      </tbody>
    </table>
  </section>

  <section>
    <h2>Anomaly Alerts — Last 10</h2>
    <table>
      <thead>
        <tr>
          <th>Timestamp</th>
          <th>Signal</th>
          <th>Value</th>
          <th>Score</th>
          <th>Severity</th>
        </tr>
      </thead>
      <tbody>
        {anomaly_rows}
      </tbody>
    </table>
  </section>

  <footer>
    sdv-edge-gateway P2.3 &nbsp;|&nbsp;
    Tables: {TABLE_TELEMETRY}, {TABLE_ANOMALIES} &nbsp;|&nbsp;
    github.com/manojbaliyan16/sdv-edge-gateway
  </footer>
</body>
</html>"""


def lambda_handler(event, context):
    """
    Lambda Function URL entry point.
    GET /          → HTML dashboard
    GET /?uin=XYZ  → dashboard filtered to that UIN
    GET /?fmt=json → raw JSON (for debugging)
    """
    qs  = event.get("queryStringParameters") or {}
    uin = qs.get("uin", TARGET_UIN)
    fmt = qs.get("fmt", "html")

    telemetry = query_latest(TABLE_TELEMETRY, uin, limit=20)
    anomalies = query_latest(TABLE_ANOMALIES, uin, limit=10)

    if fmt == "json":
        body = json.dumps(
            {"uin": uin, "telemetry": telemetry, "anomalies": anomalies},
            cls=DecimalEncoder, indent=2
        )
        return {
            "statusCode": 200,
            "headers": {"Content-Type": "application/json"},
            "body": body,
        }

    html = build_html(telemetry, anomalies, uin)
    return {
        "statusCode": 200,
        "headers": {"Content-Type": "text/html; charset=utf-8"},
        "body": html,
    }
