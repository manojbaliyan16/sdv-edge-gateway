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

POLICY_DOCUMENT = {
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Action": "iot:Connect",
            "Resource": "arn:aws:iot:*:*:client/sdv-gateway-*"
        },
        {
            "Effect": "Allow",
            "Action": "iot:Publish",
            "Resource": [
                "arn:aws:iot:*:*:topic/psa/DataCollect/from/uin/*",
                "arn:aws:iot:*:*:topic/psa/RemoteServices/from/uin/*",
                "arn:aws:iot:*:*:topic/psa/DeviceManagement/from/uin/*"
            ]
        },
        {
            "Effect": "Allow",
            "Action": "iot:Subscribe",
            "Resource": [
                "arn:aws:iot:*:*:topicfilter/psa/RemoteServices/to/uin/*",
                "arn:aws:iot:*:*:topicfilter/psa/DeviceManagement/to/uin/*"
            ]
        },
        {
            "Effect": "Allow",
            "Action": "iot:Receive",
            "Resource": "arn:aws:iot:*:*:topic/psa/*"
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
                      policyDocument=json.dumps(POLICY_DOCUMENT))
    iot.attach_policy(policyName=policy_name, target=cert_arn)
    iot.attach_thing_principal(thingName=thing_name, principal=cert_arn)

    endpoint = iot.describe_endpoint(endpointType="iot:Data-ATS")["endpointAddress"]

    print(f"\n[setup] Done!")
    print(f"  Endpoint: {endpoint}")
    print(f"  Certs written to: {args.out_dir}/")
    print(f"  Copy certs/ to Pi and update config/config.yaml → mqtt.broker: {endpoint}")

if __name__ == "__main__":
    main()
