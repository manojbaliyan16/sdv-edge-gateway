#!/usr/bin/env python3
"""
create_thing.py — One-time AWS IoT Core setup.
Creates Thing, certificates, policy, and attaches them.
Run once per device. Outputs certs/ files that go on the Pi.

Usage: python3 cloud/iot_setup/create_thing.py --uin VIN_PLACEHOLDER --region eu-west-1
"""
import boto3
import json
import os
import argparse

def build_policy_document(uin: str) -> dict:
    """
    IAM-style policy for this device's X.509 cert, scoped to ITS OWN uin only.
    Topics here MUST match the real topics used across the codebase:
      config.yaml            -> sdv/telemetry/from/uin/<uin>, sdv/commands/to/uin/<uin>,
                                 sdv/commands/from/uin/<uin>/status, sdv/ota/to/uin/<uin>/manifest,
                                 sdv/ota/from/uin/<uin>/status
      anomaly_detector.cpp    -> sdv/Analytics/from/uin/<uin>/anomaly
      command_handler.cpp     -> subscribes sdv/commands/to/uin/<uin> and
                                 sdv/Analytics/to/uin/<uin>/diagnosis
    (Previous version of this policy used sdv/DataCollect/*, sdv/RemoteServices/*,
    sdv/DeviceManagement/* — topic names that appear nowhere else in this repo. That
    mismatch would have connected fine and then silently denied every publish/subscribe,
    visible only in AWS CloudWatch, not in gateway logs. Fixed 11-Jul-26.)

    Scoped to this uin specifically (not a wildcard) so a compromised cert can't
    publish or subscribe as a different vehicle.
    """
    return {
        "Version": "2012-10-17",
        "Statement": [
            {
                "Effect": "Allow",
                "Action": "iot:Connect",
                "Resource": f"arn:aws:iot:*:*:client/sdv-gateway-{uin}"
            },
            {
                "Effect": "Allow",
                "Action": "iot:Publish",
                "Resource": [
                    f"arn:aws:iot:*:*:topic/sdv/telemetry/from/uin/{uin}",
                    f"arn:aws:iot:*:*:topic/sdv/commands/from/uin/{uin}/status",
                    f"arn:aws:iot:*:*:topic/sdv/ota/from/uin/{uin}/status",
                    f"arn:aws:iot:*:*:topic/sdv/Analytics/from/uin/{uin}/anomaly"
                ]
            },
            {
                "Effect": "Allow",
                "Action": "iot:Subscribe",
                "Resource": [
                    f"arn:aws:iot:*:*:topicfilter/sdv/commands/to/uin/{uin}",
                    f"arn:aws:iot:*:*:topicfilter/sdv/ota/to/uin/{uin}/manifest",
                    f"arn:aws:iot:*:*:topicfilter/sdv/Analytics/to/uin/{uin}/diagnosis"
                ]
            },
            {
                "Effect": "Allow",
                "Action": "iot:Receive",
                "Resource": [
                    f"arn:aws:iot:*:*:topic/sdv/commands/to/uin/{uin}",
                    f"arn:aws:iot:*:*:topic/sdv/ota/to/uin/{uin}/manifest",
                    f"arn:aws:iot:*:*:topic/sdv/Analytics/to/uin/{uin}/diagnosis"
                ]
            }
        ]
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--uin", required=True, help="Vehicle UIN / VIN")
    parser.add_argument("--region", default="eu-west-1")
    parser.add_argument("--out-dir", default="certs", help="Output directory for certs")
    args = parser.parse_args()

    iot = boto3.client("iot", region_name=args.region)
    thing_name = f"sdv-gateway-{args.uin}"
    os.makedirs(args.out_dir, exist_ok=True)

    print(f"[setup] Creating Thing: {thing_name}")
    iot.create_thing(thingName=thing_name)

    print("[setup] Creating certificate")
    cert = iot.create_keys_and_certificate(setAsActive=True)
    cert_arn = cert["certificateArn"]

    with open(f"{args.out_dir}/device.pem.crt", "w") as f:
        f.write(cert["certificatePem"])
    with open(f"{args.out_dir}/private.pem.key", "w") as f:
        f.write(cert["keyPair"]["PrivateKey"])

    print("[setup] Creating and attaching policy")
    policy_name = f"sdv-gateway-policy-{args.uin}"
    iot.create_policy(policyName=policy_name,
                      policyDocument=json.dumps(build_policy_document(args.uin)))
    iot.attach_policy(policyName=policy_name, target=cert_arn)
    iot.attach_thing_principal(thingName=thing_name, principal=cert_arn)

    endpoint = iot.describe_endpoint(endpointType="iot:Data-ATS")["endpointAddress"]

    print(f"\n[setup] Done!")
    print(f"  Endpoint: {endpoint}")
    print(f"  Certs written to: {args.out_dir}/")
    print(f"  Copy certs/ to Pi and update config/config.yaml → mqtt.broker: {endpoint}")

if __name__ == "__main__":
    main()
