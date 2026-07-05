#!/usr/bin/env python3
"""
create_infrastructure.py — One-time AWS cloud setup for sdv-edge-gateway.

Creates:
  1. DynamoDB table  "sdv_telemetry"         — stores all vehicle telemetry
  2. DynamoDB table  "sdv_anomalies"         — stores anomaly alerts
  3. IAM role        "sdv-iot-dynamo-role"   — allows IoT Core to write to DynamoDB
  4. IoT Rule        "sdv_telemetry_to_ddb"  — routes telemetry MQTT → DynamoDB
  5. IoT Rule        "sdv_anomaly_to_lambda" — routes anomaly MQTT → diagnostic Lambda

Run ONCE before Pi deploy:
  pip install boto3
  python3 cloud/iot_setup/create_infrastructure.py --region eu-west-1 [--lambda-arn arn:aws:lambda:...]

After running:
  - Go to AWS DynamoDB console → tables sdv_telemetry and sdv_anomalies will appear
  - Start the gateway on Pi → telemetry rows start appearing in real time
  - Go to AWS IoT Core → MQTT test client → subscribe to sdv/telemetry/from/uin/# to see live data
"""

import boto3
import json
import argparse
import time

# ─── Table schemas ─────────────────────────────────────────────────────────────
# sdv_telemetry: partition=uin (string), sort=timestamp (string ISO8601)
# sdv_anomalies: partition=uin (string), sort=timestamp (string ISO8601)

TABLE_TELEMETRY = "sdv_telemetry"
TABLE_ANOMALIES = "sdv_anomalies"


def create_dynamo_table(dynamo, table_name: str):
    """Create DynamoDB table with uin (PK) + timestamp (SK). Idempotent."""
    existing = [t["TableName"] for t in dynamo.list_tables()["TableNames"]]
    if table_name in existing:
        print(f"  [DynamoDB] Table '{table_name}' already exists — skipping")
        return dynamo.describe_table(TableName=table_name)["Table"]["TableArn"]

    resp = dynamo.create_table(
        TableName=table_name,
        KeySchema=[
            {"AttributeName": "uin",       "KeyType": "HASH"},    # partition key
            {"AttributeName": "timestamp", "KeyType": "RANGE"},   # sort key
        ],
        AttributeDefinitions=[
            {"AttributeName": "uin",       "AttributeType": "S"},
            {"AttributeName": "timestamp", "AttributeType": "S"},
        ],
        BillingMode="PAY_PER_REQUEST",   # no capacity planning needed for demo
        Tags=[{"Key": "project", "Value": "sdv-edge-gateway"},
              {"Key": "phase",   "Value": "P2.3"}],
    )
    arn = resp["TableDescription"]["TableArn"]
    print(f"  [DynamoDB] Created table '{table_name}'  ARN={arn}")

    # Wait for table to become active
    waiter = dynamo.get_waiter("table_exists")
    waiter.wait(TableName=table_name)
    print(f"  [DynamoDB] Table '{table_name}' is ACTIVE")
    return arn


def create_iot_role(iam, account_id: str, region: str,
                    telemetry_table_arn: str, anomaly_table_arn: str) -> str:
    """
    Create IAM role that AWS IoT Core assumes when writing to DynamoDB.
    Idempotent — returns existing role ARN if already created.
    """
    role_name = "sdv-iot-dynamo-role"

    try:
        role = iam.get_role(RoleName=role_name)
        arn  = role["Role"]["Arn"]
        print(f"  [IAM] Role '{role_name}' already exists — {arn}")
        return arn
    except iam.exceptions.NoSuchEntityException:
        pass

    trust = {
        "Version": "2012-10-17",
        "Statement": [{
            "Effect": "Allow",
            "Principal": {"Service": "iot.amazonaws.com"},
            "Action":    "sts:AssumeRole"
        }]
    }
    role = iam.create_role(
        RoleName=role_name,
        AssumeRolePolicyDocument=json.dumps(trust),
        Description="Allows AWS IoT Core to write sdv telemetry to DynamoDB",
        Tags=[{"Key": "project", "Value": "sdv-edge-gateway"}],
    )
    role_arn = role["Role"]["Arn"]

    # Inline policy: PutItem on both DynamoDB tables
    policy = {
        "Version": "2012-10-17",
        "Statement": [{
            "Effect":   "Allow",
            "Action":   ["dynamodb:PutItem"],
            "Resource": [telemetry_table_arn, anomaly_table_arn]
        }]
    }
    iam.put_role_policy(
        RoleName=role_name,
        PolicyName="sdv-iot-dynamo-write",
        PolicyDocument=json.dumps(policy),
    )

    print(f"  [IAM] Created role '{role_name}'  ARN={role_arn}")
    time.sleep(10)   # IAM propagation delay — IoT rule creation fails if role isn't ready
    return role_arn


def create_telemetry_rule(iot, role_arn: str, region: str):
    """
    IoT Rule: sdv/telemetry/from/uin/+ → DynamoDB sdv_telemetry table.

    SQL: every field in the MQTT JSON payload is stored.
    DynamoDB action uses substitution templates to set PK/SK from message.
    """
    rule_name = "sdv_telemetry_to_ddb"

    # IoT Core SQL — runs on every message matching the topic filter
    # timestamp() = message arrival epoch ms, used as sort key
    sql = "SELECT *, timestamp() as arrival_ts FROM 'sdv/telemetry/from/uin/+'"

    action = {
        "dynamoDBv2": {
            "roleArn":   role_arn,
            "putItem": {
                "tableName": TABLE_TELEMETRY
            }
        }
    }
    # DynamoDBv2 action requires uin and timestamp to be fields IN the payload.
    # TelemetryPublisher (C++) already includes uin in JSON. timestamp is added via SQL.
    # The PK/SK are auto-matched from payload fields with same name as table keys.

    try:
        iot.create_topic_rule(
            ruleName=rule_name,
            topicRulePayload={
                "sql":         sql,
                "description": "Route vehicle telemetry to DynamoDB",
                "actions":     [action],
                "ruleDisabled": False,
                "awsIotSqlVersion": "2016-03-23",
            }
        )
        print(f"  [IoT Rule] Created '{rule_name}' → DynamoDB {TABLE_TELEMETRY}")
    except iot.exceptions.ResourceAlreadyExistsException:
        print(f"  [IoT Rule] '{rule_name}' already exists — skipping")


def create_anomaly_rule(iot, role_arn: str, lambda_arn: str | None):
    """
    IoT Rule: sdv/Analytics/from/uin/+/anomaly → DynamoDB sdv_anomalies
                                                → Lambda diagnostic_agent (if ARN provided)
    """
    rule_name = "sdv_anomaly_route"
    sql       = "SELECT * FROM 'sdv/Analytics/from/uin/+/anomaly'"

    actions = [
        # Always: store anomaly in DynamoDB
        {
            "dynamoDBv2": {
                "roleArn": role_arn,
                "putItem": {"tableName": TABLE_ANOMALIES}
            }
        }
    ]

    if lambda_arn:
        # Also: invoke diagnostic_agent Lambda for RAG + LLM diagnosis
        actions.append({"lambda": {"functionArn": lambda_arn}})
        print(f"  [IoT Rule] Will also trigger Lambda: {lambda_arn}")

    try:
        iot.create_topic_rule(
            ruleName=rule_name,
            topicRulePayload={
                "sql":         sql,
                "description": "Route anomaly alerts to DynamoDB + diagnostic Lambda",
                "actions":     actions,
                "ruleDisabled": False,
                "awsIotSqlVersion": "2016-03-23",
            }
        )
        print(f"  [IoT Rule] Created '{rule_name}' → DynamoDB {TABLE_ANOMALIES}" +
              (f" + Lambda" if lambda_arn else ""))
    except iot.exceptions.ResourceAlreadyExistsException:
        print(f"  [IoT Rule] '{rule_name}' already exists — skipping")


def main():
    parser = argparse.ArgumentParser(description="Create AWS cloud infra for sdv-edge-gateway")
    parser.add_argument("--region",     default="eu-west-1")
    parser.add_argument("--lambda-arn", default=None,
                        help="ARN of deployed diagnostic_agent Lambda (optional — anomalies stored in DDB regardless)")
    args = parser.parse_args()

    session    = boto3.session.Session(region_name=args.region)
    sts        = session.client("sts")
    dynamo     = session.client("dynamodb")
    iot        = session.client("iot")
    iam        = session.client("iam")

    account_id = sts.get_caller_identity()["Account"]
    print(f"\n=== sdv-edge-gateway Cloud Setup ===")
    print(f"  Region:    {args.region}")
    print(f"  AccountId: {account_id}\n")

    print("1. Creating DynamoDB tables...")
    telemetry_arn = create_dynamo_table(dynamo, TABLE_TELEMETRY)
    anomaly_arn   = create_dynamo_table(dynamo, TABLE_ANOMALIES)

    print("\n2. Creating IAM role for IoT → DynamoDB...")
    role_arn = create_iot_role(iam, account_id, args.region, telemetry_arn, anomaly_arn)

    print("\n3. Creating IoT Rules...")
    create_telemetry_rule(iot, role_arn, args.region)
    create_anomaly_rule(iot, role_arn, args.lambda_arn)

    print(f"""
=== Setup Complete ===

Your Pi gateway can now start publishing. Verify in AWS console:

  DynamoDB:
    Tables → sdv_telemetry    (telemetry rows appear within seconds of gateway start)
    Tables → sdv_anomalies    (rows appear when anomaly_detector fires)

  IoT Core → Message Routing → Rules:
    sdv_telemetry_to_ddb   — active
    sdv_anomaly_route      — active

  IoT Core → MQTT Test Client (real-time monitor):
    Subscribe to:  sdv/telemetry/from/uin/#       (all vehicle telemetry)
    Subscribe to:  sdv/Analytics/from/uin/#        (anomaly alerts)
    Subscribe to:  sdv/Analytics/to/uin/#          (LLM diagnosis responses)

  Next step — deploy dashboard Lambda:
    cd cloud/dashboard
    zip dashboard.zip api_handler.py
    aws lambda create-function --function-name sdv-dashboard-api \\
        --runtime python3.12 --handler api_handler.lambda_handler \\
        --role <role-arn> --zip-file fileb://dashboard.zip \\
        --environment Variables={{TABLE_TELEMETRY={TABLE_TELEMETRY},TABLE_ANOMALIES={TABLE_ANOMALIES}}}
    # Then add Function URL (Lambda console → Configuration → Function URL → Auth: NONE)
    # Open that URL in browser → live telemetry dashboard
""")


if __name__ == "__main__":
    main()
