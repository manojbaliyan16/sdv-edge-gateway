"""
diagnostic_agent.py — Cloud-side LLM diagnostic agent (AWS Lambda).

CONNECTS TO:
  AGI.1 — RAG (retrieves relevant ISO 26262 DTC context for the anomaly)
  AGI.2 — Agentic AI (multi-step: detect → retrieve → diagnose → recommend → publish)

Trigger: AWS IoT Rule routes sdv/Analytics/from/uin/+/anomaly → this Lambda.

Flow (agentic pipeline):
  1. Parse anomaly event from MQTT payload
  2. Retrieve relevant DTC / ISO 26262 context from vector store (RAG)
  3. Call LLM (AWS Bedrock Claude) with: vehicle state + anomaly + retrieved context
  4. Parse LLM response: root cause + severity + recommended action
  5. Publish diagnosis back to vehicle: sdv/Analytics/to/uin/<UIN>/diagnosis

Design decision: Human-in-the-loop for CRITICAL severity.
  - WARNING  → auto-publish diagnosis to vehicle
  - CRITICAL → route to human operator first (SNS alert), then publish

This is the automotive × agentic intersection: same RAG+agent pattern as
AGI.1/AGI.2 courses, applied to a real safety-critical domain.
"""

import json
import boto3
import os
import logging
from datetime import datetime

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# AWS clients
bedrock  = boto3.client("bedrock-runtime", region_name=os.environ.get("AWS_REGION", "eu-west-1"))
iot_data = boto3.client("iot-data",        region_name=os.environ.get("AWS_REGION", "eu-west-1"))
sns      = boto3.client("sns",             region_name=os.environ.get("AWS_REGION", "eu-west-1"))

# ─── RAG retrieval (simplified — in AGI.3 project this hits a real vector store) ──
DTC_KNOWLEDGE_BASE = {
    "ENGINE_COOLANT_TEMP": {
        "dtc": "P0217",
        "desc": "Engine over temperature condition",
        "iso_ref": "ISO 26262-6 §7.4.8 — thermal runaway detection",
        "asil": "ASIL-B",
        "safe_state": "Reduce engine load, pull over safely if sustained"
    },
    "BATTERY_VOLTAGE": {
        "dtc": "P0562",
        "desc": "System voltage low",
        "iso_ref": "ISO 26262-5 §9.4.3 — power supply monitoring",
        "asil": "ASIL-A",
        "safe_state": "Graceful degradation of non-critical systems"
    },
    "VEHICLE_SPEED": {
        "dtc": "C0035",
        "desc": "Wheel speed sensor anomaly",
        "iso_ref": "ISO 26262-4 §8.4.5 — plausibility monitoring",
        "asil": "ASIL-C",
        "safe_state": "Disable ABS/ESC, alert driver"
    }
}

def retrieve_context(signal_name: str) -> dict:
    """RAG retrieval — in production this queries a FAISS/Pinecone vector store
    loaded with ISO 26262, AUTOSAR, and vehicle-specific DTC documentation."""
    # TODO (AGI.3): replace with actual vector store retrieval
    return DTC_KNOWLEDGE_BASE.get(signal_name, {
        "dtc": "U0000",
        "desc": "Unknown signal anomaly",
        "iso_ref": "ISO 26262-3 §7.4 — general safety goal monitoring",
        "asil": "QM",
        "safe_state": "Log and monitor"
    })


def call_llm(anomaly: dict, context: dict) -> str:
    """Call AWS Bedrock (Claude) with anomaly + retrieved context.
    This is the agentic step: LLM reasons over safety context to produce diagnosis."""

    prompt = f"""You are an automotive diagnostic AI assistant with expertise in ISO 26262 functional safety.

ANOMALY DETECTED:
- Signal: {anomaly['signal_name']}
- Value: {anomaly['value']} (anomaly confidence: {anomaly['anomaly_prob']:.0%})
- Severity: {anomaly['severity']}
- Vehicle: UIN={anomaly['uin']}, Speed={anomaly['vehicle_speed_kmh']} km/h

RETRIEVED SAFETY CONTEXT (RAG):
- DTC Code: {context['dtc']} — {context['desc']}
- ISO 26262 Reference: {context['iso_ref']}
- ASIL Level: {context['asil']}
- Safe State: {context['safe_state']}

Provide a concise diagnostic response in JSON with keys:
  root_cause, recommended_action, driver_message (max 20 words, plain language), escalate_human (bool)
"""

    response = bedrock.invoke_model(
        modelId="anthropic.claude-3-haiku-20240307-v1:0",
        body=json.dumps({
            "anthropic_version": "bedrock-2023-05-31",
            "max_tokens": 300,
            "messages": [{"role": "user", "content": prompt}]
        }),
        contentType="application/json",
        accept="application/json"
    )

    result = json.loads(response["body"].read())
    return result["content"][0]["text"]


def publish_diagnosis(uin: str, diagnosis: dict):
    """Publish LLM diagnosis back to vehicle over MQTT."""
    topic = f"sdv/Analytics/to/uin/{uin}/diagnosis"
    iot_data.publish(
        topic=topic,
        qos=1,
        payload=json.dumps({**diagnosis, "timestamp": datetime.utcnow().isoformat()})
    )
    logger.info(f"Published diagnosis to {topic}")


def alert_human_operator(uin: str, anomaly: dict, diagnosis: dict):
    """Human-in-the-loop: SNS alert for CRITICAL severity before publishing to vehicle.
    Pattern from AGI.2: agents with guardrails + human oversight for safety-critical actions."""
    sns.publish(
        TopicArn=os.environ["OPERATOR_SNS_TOPIC_ARN"],
        Subject=f"[CRITICAL] Vehicle {uin} — {anomaly['signal_name']} anomaly",
        Message=json.dumps({
            "uin": uin,
            "anomaly": anomaly,
            "ai_diagnosis": diagnosis,
            "action_required": "Review and confirm diagnosis before vehicle notification"
        }, indent=2)
    )
    logger.warning(f"CRITICAL anomaly — human operator alerted for UIN={uin}")


def lambda_handler(event, context):
    """
    Lambda entry point. Triggered by AWS IoT Core rule on:
    sdv/Analytics/from/uin/+/anomaly
    """
    try:
        anomaly = json.loads(event.get("payload", "{}"))
        uin = event.get("uin", anomaly.get("uin", "UNKNOWN"))
        signal_name = anomaly.get("signal_name", "")

        logger.info(f"Processing anomaly: UIN={uin}, signal={signal_name}, severity={anomaly.get('severity')}")

        # Step 1: RAG retrieval
        ctx = retrieve_context(signal_name)

        # Step 2: LLM diagnosis (agentic reasoning step)
        llm_raw = call_llm(anomaly, ctx)
        diagnosis = json.loads(llm_raw)
        diagnosis["dtc"] = ctx["dtc"]
        diagnosis["asil"] = ctx["asil"]

        # Step 3: Human-in-the-loop gate for CRITICAL
        if anomaly.get("severity") == "CRITICAL" or diagnosis.get("escalate_human"):
            alert_human_operator(uin, anomaly, diagnosis)
            return {"statusCode": 200, "body": "Escalated to human operator"}

        # Step 4: Publish diagnosis back to vehicle
        publish_diagnosis(uin, diagnosis)

        return {"statusCode": 200, "body": json.dumps(diagnosis)}

    except Exception as e:
        logger.error(f"Diagnostic agent error: {e}", exc_info=True)
        return {"statusCode": 500, "body": str(e)}
