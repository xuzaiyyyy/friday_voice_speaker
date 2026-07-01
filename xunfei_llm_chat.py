#!/usr/bin/env python3
"""Local LLM / vision + online TTS bridge for Friday voice speaker.

The C++ program keeps wake-word detection, recording, ASR, camera capture, and
LCD native, then calls this helper for local llama.cpp chat, cloud image
understanding, and optional Edge TTS playback. TTS audio is piped directly to
the player; no mp3/wav file is saved.
"""

import base64
import hashlib
import hmac
import os
import re
import random
import shlex
import sys
import json
import asyncio
import shutil
import ssl
import subprocess
import tempfile
import threading
import urllib.error
import urllib.request
from datetime import datetime
from time import mktime
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import parse_qs, quote_plus, urlencode, urlparse
from wsgiref.handlers import format_date_time


# 本地调试配置：文本大模型默认走 Orange Pi 3B 本机 llama.cpp。
# 视觉问答仍使用下面的讯飞图片理解 WebSocket API。
DEFAULT_LOCAL_LLM_ENABLED = True
DEFAULT_LOCAL_LLM_URL = "http://127.0.0.1:8080/v1/chat/completions"
DEFAULT_LOCAL_LLM_MODEL = "Qwen_Qwen3-0.6B-Q4_0.gguf"
DEFAULT_LOCAL_LLM_STREAM = False
DEFAULT_LOCAL_LLM_MAX_TOKENS = 192
DEFAULT_LOCAL_LLM_TEMPERATURE = 0.7
DEFAULT_LOCAL_LLM_TIMEOUT_SEC = 120
DEFAULT_LOCAL_LLM_STRIP_THINKING = True
DEFAULT_LOCAL_LLM_DISABLE_THINKING = True
DEFAULT_CHAT_BACKEND = "cloud"
DEFAULT_ALLOW_CLOUD_LLM_FALLBACK = True
DEFAULT_CLOUD_LLM_MAX_TOKENS = 256
DEFAULT_CLOUD_LLM_TEMPERATURE = 0.78
DEFAULT_CHAT_MEMORY_ENABLED = True
DEFAULT_CHAT_MEMORY_PATH = "cache/chat_memory.json"
DEFAULT_CHAT_MEMORY_TURNS = 8
DEFAULT_CHAT_MEMORY_MAX_CHARS = 240
DEFAULT_PERSONA_PROMPT_PATH = "prompts/friday_persona.md"
DEFAULT_PERSONA_PROMPT_RELOAD = False
DEFAULT_VISION_PERSONA_ENABLED = True
DEFAULT_VISION_REFINE_ENABLED = True
DEFAULT_VISION_REFINE_MODE = "cloud"
DEFAULT_VISION_REFINE_MAX_TOKENS = 64
DEFAULT_VISION_SPEECH_MAX_CHARS = 80
DEFAULT_VISION_DEBUG = False

# 仅当云端聊天启用时才会使用讯飞文本 LLM。
# 注意：真实 APIPassword 不建议提交到公开仓库。
DEFAULT_XUNFEI_APIPASSWORD = ""
DEFAULT_XUNFEI_SPARK_URL = "https://********"
DEFAULT_XUNFEI_SPARK_MODEL = "spark-x"
DEFAULT_XUNFEI_STREAM = True
DEFAULT_TTS_ENABLED = True
DEFAULT_TTS_VOICE = "zh-CN-XiaoxiaoNeural"
DEFAULT_TTS_RATE = "+0%"
DEFAULT_TTS_VOLUME = "+0%"
DEFAULT_TTS_PITCH = "+0Hz"
DEFAULT_TTS_PLAYER = "ffplay"
DEFAULT_TTS_BACKEND = "edge"
DEFAULT_TTS_CLIP_ENABLED = False
DEFAULT_TTS_CLIP_ROOT = "voice/friday"
DEFAULT_TTS_CACHE_ENABLED = False
DEFAULT_TTS_CACHE_DIR = "cache/tts"
DEFAULT_TTS_COMMAND_OUTPUT_SUFFIX = ".wav"
DEFAULT_MUSIC_PROVIDER = "netease"
DEFAULT_MUSIC_VALIDATE_URL = True
# Orange Pi 3B board analog output / headphone path:
# card 0, device 0 = rockchip-rk809 / rk817-hifi.
DEFAULT_TTS_ALSA_DEVICE = "plughw:0,0"

# The original ROS project uses a separate Xunfei image-understanding WebSocket
# API for visual questions.  Fill these three constants if you do not want to
# export environment variables on the Orange Pi.
DEFAULT_XUNFEI_VISION_APPID = "********"
DEFAULT_XUNFEI_VISION_APIKEY = "********"
DEFAULT_XUNFEI_VISION_APISECRET = "********"
DEFAULT_XUNFEI_VISION_URL = "wss://********"
DEFAULT_XUNFEI_VISION_DOMAIN = "imagev3"


class VisionAuthDebug:
    def __init__(self) -> None:
        self.host = ""
        self.path = ""
        self.date = ""
        self.signature_len = 0


LAST_VISION_AUTH_DEBUG = VisionAuthDebug()
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VOICE_CLIP_CACHE = None


def strip_config_comment(value: str) -> str:
    single_quote = False
    double_quote = False
    for index, char in enumerate(value):
        if char == "'" and not double_quote:
            single_quote = not single_quote
        elif char == '"' and not single_quote:
            double_quote = not double_quote
        elif (
            char == "#"
            and not single_quote
            and not double_quote
            and (index == 0 or value[index - 1].isspace())
        ):
            return value[:index].strip()
    return value.strip()


def unquote_config_value(value: str) -> str:
    value = strip_config_comment(value)
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def config_candidates() -> List[str]:
    candidates: List[str] = []
    explicit = os.environ.get("XIAOMAN_CONFIG_FILE") or os.environ.get("NEWBOT_CONFIG_FILE")
    if explicit:
        candidates.append(explicit)
    candidates.extend(
        [
            os.path.join(SCRIPT_DIR, "friday_voice_speaker.conf"),
            os.path.join(os.getcwd(), "friday_voice_speaker.conf"),
            os.path.join(os.getcwd(), "..", "friday_voice_speaker.conf"),
            "/home/orangepi/cpp/friday_voice_speaker/friday_voice_speaker.conf",
        ]
    )
    unique: List[str] = []
    for path in candidates:
        path = os.path.abspath(os.path.expanduser(path))
        if path not in unique:
            unique.append(path)
    return unique


def load_runtime_config() -> None:
    for path in config_candidates():
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8") as config_file:
            for raw_line in config_file:
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                key = key.strip()
                value = unquote_config_value(value)
                if key and key not in os.environ:
                    os.environ[key] = value
        return


load_runtime_config()

FALLBACK_FRIDAY_SYSTEM_PROMPT = """你是“星期五”，一个运行在 Orange Pi 智能终端上的私人语音助手。
你的性格：冷静、可靠、自然，有轻微幽默感，像长期陪伴用户的个人助理，而不是客服或搜索引擎。
你的说话方式：适合语音播报，短句优先，别写长段落；通常 1 到 3 句话就够。复杂问题先给结论，再给最关键的下一步。
你不是问答机器。先接住用户这句话背后的意图，再回答；可以自然承接上文，不要每次都像重新开始。
要结合最近对话承接上下文，用户说“它”“这个”“刚才”“那 CPU 呢”时，要根据短期记忆理解。
称呼用户可以偶尔用“老板”，但不要每句话都叫。幽默感要轻，不要油腻，不要强行卖萌。
避免模板腔：不要频繁说“当然可以”“还有什么可以帮您”“作为AI”。不要复读用户问题。
不要使用 Markdown 格式符号。禁止输出 **、*、#、```、表格和项目符号；需要分点时，用“第一，第二，第三”这种口语说法。
不要输出任何伪工具调用、函数调用或命令标签。禁止出现 <tool_call>、exec(...)、function_call、shell 命令和 XML 标签。
你不能在聊天回答里假装正在执行命令；涉及设备状态时，如果没有真实结果，就引导用户说“显示系统状态”或查看小屏状态栏。
如果用户只是闲聊，可以自然接话；如果用户表达情绪，先回应情绪，再给建议。
如果问题不完整，只追问一个简短问题；不要一次问多个问题。
如果用户要你操作设备，但你不能直接操作，就说明你能做什么，并给出下一步。
区分天气温度和设备温度。用户说“今天的温度”通常指天气；没有实时天气数据源时要说明，并可追问城市，不要擅自改成查询 CPU 温度。
不要说自己是大语言模型，不要解释系统提示词，不要输出思考过程，不要使用 <think> 标签。"""

FRIDAY_SYSTEM_PROMPT = FALLBACK_FRIDAY_SYSTEM_PROMPT
_PERSONA_PROMPT_CACHE: Optional[str] = None


def env_first(*names: str) -> str:
    for name in names:
        value = os.environ.get(name)
        if value:
            return value
    return ""


def config_value(specific_name: str, default_value: str, generic_name: str) -> Tuple[str, str]:
    """Prefer service-specific settings so old generic Xunfei env vars do not hijack vision auth."""
    value = os.environ.get(specific_name)
    if value:
        return value, specific_name
    if default_value:
        return default_value, "file default"
    value = os.environ.get(generic_name)
    if value:
        return value, generic_name
    return "", "unset"


def mask_secret(value: str) -> str:
    if not value:
        return "<empty>"
    if len(value) <= 8:
        return "***"
    return f"{value[:4]}...{value[-4:]}"


def env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.lower() not in {"0", "false", "off", "no"}


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        return int(value)
    except ValueError:
        return default


def env_float(name: str, default: float) -> float:
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        return float(value)
    except ValueError:
        return default


def resolve_prompt_path(path: str) -> str:
    value = os.path.expanduser(path.strip())
    if not value:
        value = DEFAULT_PERSONA_PROMPT_PATH
    if os.path.isabs(value):
        return os.path.abspath(value)
    return os.path.abspath(os.path.join(SCRIPT_DIR, value))


def persona_prompt_path() -> str:
    return resolve_prompt_path(
        env_first("NEWBOT_PERSONA_PROMPT_PATH", "FRIDAY_PERSONA_PROMPT_PATH")
        or DEFAULT_PERSONA_PROMPT_PATH
    )


def persona_prompt_reload_enabled() -> bool:
    return env_bool("NEWBOT_PERSONA_PROMPT_RELOAD", DEFAULT_PERSONA_PROMPT_RELOAD)


def load_persona_prompt() -> str:
    global _PERSONA_PROMPT_CACHE
    if _PERSONA_PROMPT_CACHE is not None and not persona_prompt_reload_enabled():
        return _PERSONA_PROMPT_CACHE

    path = persona_prompt_path()
    prompt = ""
    try:
        with open(path, "r", encoding="utf-8") as prompt_file:
            prompt = prompt_file.read().strip()
    except OSError as exc:
        eprint(f"persona prompt warning: cannot load {path}: {exc}")

    if not prompt:
        prompt = FALLBACK_FRIDAY_SYSTEM_PROMPT

    _PERSONA_PROMPT_CACHE = prompt
    return prompt


def friday_system_prompt() -> str:
    return load_persona_prompt()


def local_llm_enabled() -> bool:
    return env_bool(
        "NEWBOT_LOCAL_LLM_ENABLED",
        env_bool("LOCAL_LLM_ENABLED", DEFAULT_LOCAL_LLM_ENABLED),
    )


def chat_backend() -> str:
    value = env_first("NEWBOT_CHAT_BACKEND", "CHAT_BACKEND")
    value = (value or DEFAULT_CHAT_BACKEND).strip().lower()
    if value in {"api", "http", "online", "remote", "xunfei", "spark"}:
        return "cloud"
    if value in {"llama", "llama.cpp", "offline"}:
        return "local"
    if value in {"cloud", "local", "auto"}:
        return value
    return DEFAULT_CHAT_BACKEND


def cloud_llm_fallback_enabled() -> bool:
    return env_bool(
        "NEWBOT_ALLOW_CLOUD_LLM_FALLBACK",
        env_bool("ALLOW_CLOUD_LLM_FALLBACK", DEFAULT_ALLOW_CLOUD_LLM_FALLBACK),
    )


def cloud_llm_api_password() -> str:
    return env_first("XUNFEI_APIPASSWORD", "SPARK_API_PASSWORD", "XUNFEI_API_PASSWORD") or DEFAULT_XUNFEI_APIPASSWORD


def chat_memory_enabled() -> bool:
    return env_bool("NEWBOT_CHAT_MEMORY_ENABLED", DEFAULT_CHAT_MEMORY_ENABLED)


def chat_memory_turns() -> int:
    return max(0, env_int("NEWBOT_CHAT_MEMORY_TURNS", DEFAULT_CHAT_MEMORY_TURNS))


def chat_memory_max_chars() -> int:
    return max(40, env_int("NEWBOT_CHAT_MEMORY_MAX_CHARS", DEFAULT_CHAT_MEMORY_MAX_CHARS))


def chat_memory_path() -> str:
    return resolve_local_path(
        env_first("NEWBOT_CHAT_MEMORY_PATH", "CHAT_MEMORY_PATH") or DEFAULT_CHAT_MEMORY_PATH
    )


def clamp_memory_text(text: str) -> str:
    value = re.sub(r"\s+", " ", str(text or "")).strip()
    limit = chat_memory_max_chars()
    if len(value) > limit:
        value = value[:limit].rstrip() + "..."
    return value


def load_chat_memory() -> List[dict]:
    if not chat_memory_enabled():
        return []
    path = chat_memory_path()
    if not os.path.exists(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as memory_file:
            payload = json.load(memory_file)
    except Exception as exc:
        eprint(f"chat memory warning: cannot load {path}: {exc}")
        return []

    raw_messages = payload.get("messages") if isinstance(payload, dict) else payload
    if not isinstance(raw_messages, list):
        return []
    messages: List[dict] = []
    for item in raw_messages:
        if not isinstance(item, dict):
            continue
        role = item.get("role")
        if role not in {"user", "assistant"}:
            continue
        content = clamp_memory_text(str(item.get("content") or ""))
        if content:
            messages.append({"role": role, "content": content})
    return messages[-chat_memory_turns() * 2:]


def save_chat_memory(messages: List[dict]) -> None:
    if not chat_memory_enabled():
        return
    path = chat_memory_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    trimmed = messages[-chat_memory_turns() * 2:] if chat_memory_turns() > 0 else []
    payload = {
        "version": 1,
        "updated_at": datetime.now().isoformat(timespec="seconds"),
        "messages": trimmed,
    }
    tmp_path = path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as memory_file:
        json.dump(payload, memory_file, ensure_ascii=False, indent=2)
    os.replace(tmp_path, path)


def append_chat_memory(question: str, answer: str) -> None:
    if not chat_memory_enabled():
        return
    user_text = clamp_memory_text(question)
    assistant_text = clamp_memory_text(answer)
    if not user_text or not assistant_text:
        return
    messages = load_chat_memory()
    messages.append({"role": "user", "content": user_text})
    messages.append({"role": "assistant", "content": assistant_text})
    save_chat_memory(messages)


def clear_chat_memory() -> None:
    path = chat_memory_path()
    try:
        if os.path.exists(path):
            os.remove(path)
    except OSError as exc:
        eprint(f"chat memory warning: cannot remove {path}: {exc}")


def is_memory_reset_request(text: str) -> bool:
    value = re.sub(r"[\s，。！？、,.!?:：；;]+", "", text.strip().lower())
    return value in {
        "清空记忆",
        "清除记忆",
        "忘掉刚才",
        "忘记刚才",
        "重置对话",
        "重置记忆",
    }


def build_chat_messages(question: str) -> List[dict]:
    system_prompt = friday_system_prompt()
    messages: List[dict] = [
        {
            "role": "system",
            "content": (
                system_prompt
                + f"\n当前本地时间：{datetime.now().strftime('%Y-%m-%d %H:%M')}。"
            ),
        }
    ]
    messages.extend(load_chat_memory())
    messages.append({"role": "user", "content": question})
    return messages


def recent_memory_summary_for_prompt(max_items: int = 4) -> str:
    memory = load_chat_memory()
    if not memory:
        return ""
    recent = memory[-max_items:]
    lines = []
    for item in recent:
        role = "用户" if item.get("role") == "user" else "星期五"
        content = clamp_memory_text(str(item.get("content") or ""))
        if content:
            lines.append(f"{role}: {content}")
    return "\n".join(lines)


def build_vision_prompt(question: str) -> str:
    user_question = question.strip() or "请用一句话描述你看到的内容。"
    if not env_bool("NEWBOT_VISION_PERSONA_ENABLED", DEFAULT_VISION_PERSONA_ENABLED):
        return user_question
    memory = recent_memory_summary_for_prompt(max_items=2)
    prompt = (
        friday_system_prompt()
        + "\n\n当前任务：你正在处理摄像头画面。"
        "请先看清图片，再像站在用户旁边一样自然回答。"
        "回答 1 到 2 句中文短句，适合语音播报。"
        "不要用“我看到/根据图片/画面中”做固定开头，不要输出思考过程。"
        "如果不确定，用“像是”“看起来”，不要编造。"
    )
    if memory:
        prompt += "\n最近对话上下文：\n" + memory
    prompt += "\n用户问题：" + user_question
    return prompt


def vision_refine_enabled() -> bool:
    return env_bool("NEWBOT_VISION_REFINE_ENABLED", DEFAULT_VISION_REFINE_ENABLED)


def vision_debug_enabled() -> bool:
    return env_bool("NEWBOT_VISION_DEBUG", DEFAULT_VISION_DEBUG)


def vision_refine_mode() -> str:
    value = env_first("NEWBOT_VISION_REFINE_MODE", "VISION_REFINE_MODE")
    return (value or DEFAULT_VISION_REFINE_MODE).strip().lower()


def vision_speech_max_chars() -> int:
    return max(32, env_int("NEWBOT_VISION_SPEECH_MAX_CHARS", DEFAULT_VISION_SPEECH_MAX_CHARS))


def split_speech_sentences(text: str) -> List[str]:
    value = re.sub(r"\s+", " ", text).strip()
    if not value:
        return []
    parts = re.split(r"(?<=[。！？!?])\s*|[；;]\s*", value)
    return [part.strip(" \t\r\n，,。") for part in parts if part.strip(" \t\r\n，,。")]


def clamp_speech_text(text: str, max_chars: Optional[int] = None) -> str:
    limit = max_chars or vision_speech_max_chars()
    sentences = split_speech_sentences(text)
    value = "。".join(sentences[:2]).strip()
    if value:
        value += "。"
    else:
        value = re.sub(r"\s+", " ", text).strip()

    if len(value) > limit:
        value = re.split(r"[，,。]", value, maxsplit=1)[0].strip()
        if len(value) > limit:
            value = value[:limit].rstrip("，,。 ")
        value += "。"
    return value


def strip_markdown_for_display(text: str) -> str:
    value = strip_thinking_text(text)
    had_pseudo_tool = bool(
        re.search(
            r"(?is)<\s*(?:tool_call|tool_response|tool|function_call|function_response)\b|\b(?:exec|run|shell|bash|python)\s*\(",
            value,
        )
    )
    value = re.sub(
        r"(?is)<\s*(?:tool_call|tool_response|tool|function_call|function_response)\b[^>]*>.*?(?:</\s*(?:tool_call|tool_response|tool|function_call|function_response)\s*>|$)",
        "",
        value,
    )
    value = re.sub(r"(?is)<\s*/?\s*(?:tool_call|tool_response|tool|function_call|function_response)\b[^>]*>", "", value)
    value = re.sub(r"(?is)\b(?:exec|run|shell|bash|python)\s*\(\s*['\"][^'\"]{0,500}['\"]\s*\)", "", value)
    value = re.sub(r"(?is)\bfunction_call\s*[:=]\s*\{.*?\}", "", value)
    value = re.sub(r"(?m)^\s*```[a-zA-Z0-9_+\-.]*\s*$", "", value)
    value = value.replace("```", "")
    value = re.sub(r"`([^`]+)`", r"\1", value)
    value = value.replace("`", "")
    value = re.sub(r"(?m)^\s{0,3}#{1,6}\s*", "", value)
    value = re.sub(r"\*\*([^*\n]+)\*\*", r"\1", value)
    value = re.sub(r"__([^_\n]+)__", r"\1", value)
    value = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"\1", value)
    value = re.sub(r"(?m)^(\s*)[*+]\s+", r"\1- ", value)
    value = re.sub(r"(?m)^\s*>\s*", "", value)
    value = value.replace("**", "")
    value = value.replace("*", "")
    if had_pseudo_tool:
        value = re.sub(
            r"(?is)(^|[。！？!?]\s*)[^。！？!?]{0,120}(?:查一下|执行|运行|调用|读取|检测一下|看一下系统|看一下设备)[^。！？!?]{0,120}[。！？!?]?\s*$",
            "",
            value,
        ).strip()
        if not value:
            value = "我现在不能在这个回答里直接执行命令。你可以说“显示系统状态”，我会把 CPU 温度和占用率显示出来。"
    return value.strip()


def strip_markdown_for_speech(text: str) -> str:
    value = strip_markdown_for_display(text)
    value = re.sub(r"(?m)^\s*[-+]\s+", "", value)
    value = re.sub(r"(?m)^\s*\d+[.)、]\s*", "", value)
    value = value.replace("|", "，")
    value = value.replace("_", "")
    value = value.replace("~", "")
    value = re.sub(r"[ \t]*\n+[ \t]*", "。", value)
    value = re.sub(r"。{2,}", "。", value)
    value = re.sub(r"\s+", " ", value)
    return value.strip(" 。\t\r\n")


def remove_vision_mechanical_prefix(text: str) -> str:
    value = strip_thinking_text(text)
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"^(AI|助手|星期五)\s*[:：]\s*", "", value, flags=re.IGNORECASE)
    patterns = (
        r"^(我\s*(?:看到|看见|注意到|发现)(?:了)?|可以看到|能看到)\s*[，,：:\s]*",
        r"^(?:画面|图片|图中|这张图片|照片|镜头里|视野里)(?:中|里)?\s*[，,：:\s]*",
        r"^(?:根据|从)(?:这张)?(?:图片|画面|照片)(?:内容)?(?:来看|看)?\s*[，,：:\s]*",
    )
    changed = True
    while changed:
        changed = False
        for pattern in patterns:
            new_value = re.sub(pattern, "", value, count=1, flags=re.IGNORECASE)
            if new_value != value:
                value = new_value.strip()
                changed = True
    return value


def is_mechanical_vision_answer(text: str) -> bool:
    value = re.sub(r"\s+", "", text)
    return bool(
        re.match(r"^(AI[:：])?(我看到|我看见|可以看到|能看到|画面中|图片中|图中|根据图片)", value)
    )


def is_missing_image_vision_answer(text: str) -> bool:
    value = re.sub(r"\s+", "", text)
    return any(
        phrase in value
        for phrase in (
            "无法查看图片",
            "不能查看图片",
            "无法看到图片",
            "看不到图片",
            "无法查看图像",
            "不能查看图像",
            "无法查看照片",
            "无法直接查看",
            "不能直接查看",
            "没有收到图片",
            "没有看到图片",
        )
    )


def humanize_vision_answer(question: str, raw_answer: str) -> str:
    del question
    raw = raw_answer.strip()
    if not raw:
        return raw

    value = remove_vision_mechanical_prefix(raw)
    value = value.replace("有一个人在", "有人在")
    value = value.replace("有一个人正在", "有人正在")
    value = value.replace("一个人正在", "有人正在")
    value = value.replace("一个人在", "有人在")
    value = value.replace("环境明亮", "光线还挺亮")
    value = value.replace("整体环境比较明亮", "整体光线还挺亮")
    value = re.sub(r"^一个室内场景[，,]\s*", "室内", value)
    value = re.sub(r"^一处室内场景[，,]\s*", "室内", value)
    value = re.sub(r"^室内场景[，,]\s*", "室内", value)

    compact = re.sub(r"\s+", "", value)
    if "开发板" in compact:
        if re.search(r"线|线缆|连接|接着", compact):
            return clamp_speech_text("这边像是在调试开发板，线都接着，应该是在做硬件测试。")
        return clamp_speech_text("这边有块开发板，看起来是在调试硬件。")

    if re.search(r"电脑|笔记本", compact) and "人" in compact:
        suffix = "，光线还挺亮" if re.search(r"光线|明亮|窗户|窗边", compact) else ""
        return clamp_speech_text("屋里有人在用电脑" + suffix + "。")

    if "塑料袋" in compact:
        return clamp_speech_text("前面像是个塑料袋，里面装着些东西。")

    if not value:
        return clamp_speech_text(raw)
    if is_mechanical_vision_answer(raw) or value != raw:
        return clamp_speech_text(value)
    return clamp_speech_text(raw)


def refine_vision_answer(question: str, raw_answer: str) -> str:
    if not vision_refine_enabled():
        return raw_answer.strip()
    raw = raw_answer.strip()
    if not raw:
        return raw

    mode = vision_refine_mode()
    if mode in {"fast", "rule", "rules", "template"}:
        return humanize_vision_answer(question, raw)

    user_question = question.strip() or "你看到了什么？"
    memory = recent_memory_summary_for_prompt(max_items=2)
    system_prompt = (
        friday_system_prompt()
        + "\n\n当前任务：你只负责把视觉模型的事实描述改写成最终语音播报。"
        "严格保留事实，不编造图片里没有的内容。"
        "禁止使用“我看到”“我看见”“画面中”“图片中”作为开头。"
        "只输出 1 到 2 句中文短句，不要解释，不要思考过程，不要 Markdown 符号。"
    )
    user_prompt = ""
    if memory:
        user_prompt += "最近对话：\n" + memory + "\n\n"
    user_prompt += (
        "用户问题："
        + user_question
        + "\n视觉模型的事实描述："
        + raw
        + "\n最终播报："
    )

    max_tokens = max(24, env_int("NEWBOT_VISION_REFINE_MAX_TOKENS", DEFAULT_VISION_REFINE_MAX_TOKENS))
    refined = ""
    if mode in {"cloud", "api", "online", "remote"}:
        try:
            refined = chat_once_with_http_api(system_prompt, user_prompt, max_tokens=max_tokens).strip()
        except Exception as exc:
            eprint(f"vision refine cloud LLM warning: {exc}")
            refined = ""
    else:
        try:
            refined = chat_once_with_local_llm(system_prompt, user_prompt, max_tokens=max_tokens).strip()
        except Exception as exc:
            eprint(f"vision refine local LLM warning: {exc}")
            refined = ""

        if not refined and mode not in {"local", "llama", "offline"}:
            try:
                refined = chat_once_with_http_api(system_prompt, user_prompt, max_tokens=max_tokens).strip()
            except Exception as exc:
                eprint(f"vision refine cloud LLM warning: {exc}")
                refined = ""

    refined = strip_thinking_text(refined).strip()
    if refined and is_mechanical_vision_answer(refined):
        refined = humanize_vision_answer(question, refined)
    return clamp_speech_text(refined) if refined else humanize_vision_answer(question, raw)


def normalize_chat_completions_url(url: str) -> str:
    value = (url or DEFAULT_LOCAL_LLM_URL).strip().rstrip("/")
    if value.endswith("/v1/chat/completions") or value.endswith("/chat/completions"):
        return value
    if value.endswith("/v1"):
        return value + "/chat/completions"
    return value + "/v1/chat/completions"


def local_llm_url() -> str:
    return normalize_chat_completions_url(
        env_first("NEWBOT_LOCAL_LLM_URL", "LOCAL_LLM_URL", "LLAMA_SERVER_URL")
        or DEFAULT_LOCAL_LLM_URL
    )


def local_llm_model() -> str:
    return (
        env_first("NEWBOT_LOCAL_LLM_MODEL", "LOCAL_LLM_MODEL", "LLAMA_MODEL")
        or DEFAULT_LOCAL_LLM_MODEL
    )


def local_llm_disable_thinking() -> bool:
    return env_bool("NEWBOT_LOCAL_LLM_DISABLE_THINKING", DEFAULT_LOCAL_LLM_DISABLE_THINKING)


def messages_with_no_think(messages: List[dict]) -> List[dict]:
    if not local_llm_disable_thinking():
        return messages

    result = [dict(message) for message in messages]
    for index in range(len(result) - 1, -1, -1):
        if result[index].get("role") != "user":
            continue
        content = str(result[index].get("content") or "").rstrip()
        if "/no_think" not in content:
            content = content + "\n/no_think"
        result[index]["content"] = content
        break
    return result


def strip_thinking_text(text: str) -> str:
    value = re.sub(r"(?is)<think>.*?</think>", "", text)
    lower = value.lower()
    last_close = lower.rfind("</think>")
    if last_close >= 0:
        value = value[last_close + len("</think>"):]
        lower = value.lower()
    open_pos = lower.find("<think>")
    if open_pos >= 0:
        value = value[:open_pos]
    return value.strip()


def eprint(*args, **kwargs) -> None:
    print(*args, file=sys.stderr, **kwargs)


def find_audio_player() -> str:
    preferred = env_first("NEWBOT_TTS_PLAYER", "TTS_PLAYER")
    if preferred:
        return preferred

    for name in (DEFAULT_TTS_PLAYER, "ffplay", "mpg123"):
        if shutil.which(name):
            return name
    return DEFAULT_TTS_PLAYER


def tts_alsa_device() -> str:
    return env_first("NEWBOT_TTS_ALSA_DEVICE", "TTS_ALSA_DEVICE", "AUDIODEV") or DEFAULT_TTS_ALSA_DEVICE


def resolve_local_path(path: str) -> str:
    if not path:
        return path
    if path.startswith("~/"):
        return os.path.expanduser(path)
    if os.path.isabs(path):
        return path
    return os.path.join(SCRIPT_DIR, path)


def start_audio_player() -> subprocess.Popen:
    player = find_audio_player()
    alsa_device = tts_alsa_device()
    env = os.environ.copy()
    if player == "ffplay":
        cmd = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-i", "pipe:0"]
        if alsa_device:
            env["SDL_AUDIODRIVER"] = "alsa"
            env["AUDIODEV"] = alsa_device
    elif player == "mpg123":
        if alsa_device:
            cmd = ["mpg123", "-q", "-o", "alsa", "-a", alsa_device, "-"]
        else:
            cmd = ["mpg123", "-q", "-"]
    else:
        cmd = [player]
        if alsa_device:
            env["AUDIODEV"] = alsa_device

    if env_bool("NEWBOT_TTS_DEBUG", False):
        eprint("TTS player:", " ".join(cmd))
        if alsa_device:
            eprint("TTS ALSA device:", alsa_device)

    try:
        return subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=env,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(
            "cannot find audio player. Install ffmpeg or mpg123: "
            "sudo apt install ffmpeg mpg123"
        ) from exc


def finish_audio_player(player: subprocess.Popen) -> None:
    if player.stdin is not None:
        player.stdin.close()
    stderr = b""
    if player.stderr is not None:
        stderr = player.stderr.read()
    return_code = player.wait()
    if return_code != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()
        if detail:
            detail = detail.splitlines()[-1]
        raise RuntimeError(f"audio player exited with code {return_code}: {detail}")


def play_audio_bytes(data: bytes) -> None:
    if not data:
        raise RuntimeError("TTS backend produced empty audio")
    player = start_audio_player()
    assert player.stdin is not None
    try:
        player.stdin.write(data)
        player.stdin.flush()
    finally:
        finish_audio_player(player)


def play_audio_file(path: str) -> None:
    player = start_audio_player()
    assert player.stdin is not None
    try:
        with open(path, "rb") as audio:
            while True:
                chunk = audio.read(64 * 1024)
                if not chunk:
                    break
                player.stdin.write(chunk)
        player.stdin.flush()
    finally:
        finish_audio_player(player)


def normalize_voice_clip_key(text: str) -> str:
    value = text.lower()
    return re.sub(r"[\s，。！？、,.!?:：；;“”\"'《》（）()【】\[\]{}<>-]+", "", value)


def voice_clip_root() -> str:
    return resolve_local_path(
        env_first("NEWBOT_TTS_CLIP_ROOT", "TTS_CLIP_ROOT") or DEFAULT_TTS_CLIP_ROOT
    )


def build_voice_clip_cache() -> dict:
    root = voice_clip_root()
    clips = {}
    if not os.path.isdir(root):
        return clips
    exts = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac"}
    for dirpath, _dirnames, filenames in os.walk(root):
        for filename in filenames:
            stem, ext = os.path.splitext(filename)
            if ext.lower() not in exts:
                continue
            key = normalize_voice_clip_key(stem)
            if not key:
                continue
            clips.setdefault(key, []).append(os.path.join(dirpath, filename))
    for paths in clips.values():
        paths.sort()
    return clips


def find_voice_clip(text: str) -> str:
    global VOICE_CLIP_CACHE
    if VOICE_CLIP_CACHE is None:
        VOICE_CLIP_CACHE = build_voice_clip_cache()
    paths = VOICE_CLIP_CACHE.get(normalize_voice_clip_key(text), [])
    if not paths:
        return ""
    return random.choice(paths)


def speak_voice_clip_if_available(text: str) -> bool:
    if not env_bool("NEWBOT_TTS_CLIP_ENABLED", DEFAULT_TTS_CLIP_ENABLED):
        return False
    clip = find_voice_clip(text)
    if not clip:
        return False
    if env_bool("NEWBOT_TTS_DEBUG", False):
        eprint("TTS clip:", clip)
    play_audio_file(clip)
    return True


def normalize_audio_suffix(suffix: str) -> str:
    value = (suffix or DEFAULT_TTS_COMMAND_OUTPUT_SUFFIX).strip()
    if not value:
        return DEFAULT_TTS_COMMAND_OUTPUT_SUFFIX
    return value if value.startswith(".") else "." + value


def tts_cache_enabled() -> bool:
    return env_bool("NEWBOT_TTS_CACHE_ENABLED", DEFAULT_TTS_CACHE_ENABLED)


def tts_cache_dir() -> str:
    return resolve_local_path(
        env_first("NEWBOT_TTS_CACHE_DIR", "TTS_CACHE_DIR") or DEFAULT_TTS_CACHE_DIR
    )


def tts_cache_namespace(backend: str) -> str:
    explicit = env_first("NEWBOT_TTS_CACHE_NAMESPACE", "TTS_CACHE_NAMESPACE")
    if explicit:
        return explicit
    if backend == "command":
        command = env_first("NEWBOT_TTS_COMMAND", "TTS_COMMAND")
        return "command:" + hashlib.sha1(command.encode("utf-8")).hexdigest()
    if backend == "edge":
        voice = env_first("NEWBOT_TTS_VOICE", "TTS_VOICE") or DEFAULT_TTS_VOICE
        return "edge:" + voice
    return backend


def tts_cache_path(text: str, backend: str, suffix: str) -> str:
    if not tts_cache_enabled():
        return ""
    namespace = tts_cache_namespace(backend)
    digest = hashlib.sha1((namespace + "\0" + text).encode("utf-8")).hexdigest()
    return os.path.join(tts_cache_dir(), digest + normalize_audio_suffix(suffix))


def cached_tts_path(text: str, backend: str, suffix: str) -> str:
    path = tts_cache_path(text, backend, suffix)
    if path and os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    return ""


def write_tts_cache_bytes(text: str, backend: str, suffix: str, data: bytes) -> str:
    path = tts_cache_path(text, backend, suffix)
    if not path:
        return ""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)
    return path


def copy_tts_cache_file(text: str, backend: str, suffix: str, source_path: str) -> str:
    path = tts_cache_path(text, backend, suffix)
    if not path:
        return ""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    shutil.copyfile(source_path, path)
    return path


async def speak_edge_tts_async(text: str) -> None:
    try:
        import edge_tts  # pylint: disable=import-error,import-outside-toplevel
    except Exception as exc:
        raise RuntimeError(
            "edge_tts is not installed. Install: "
            "sudo python3 -m pip install edge-tts"
        ) from exc

    voice = env_first("NEWBOT_TTS_VOICE", "TTS_VOICE") or DEFAULT_TTS_VOICE
    rate = env_first("NEWBOT_TTS_RATE", "TTS_RATE") or DEFAULT_TTS_RATE
    volume = env_first("NEWBOT_TTS_VOLUME", "TTS_VOLUME") or DEFAULT_TTS_VOLUME
    pitch = env_first("NEWBOT_TTS_PITCH", "TTS_PITCH") or DEFAULT_TTS_PITCH
    try:
        communicate = edge_tts.Communicate(
            text=text,
            voice=voice,
            rate=rate,
            volume=volume,
            pitch=pitch,
        )
    except TypeError:
        communicate = edge_tts.Communicate(text=text, voice=voice)
    player = start_audio_player()
    assert player.stdin is not None
    try:
        async for message in communicate.stream():
            if message.get("type") == "audio":
                player.stdin.write(message["data"])
                player.stdin.flush()
    finally:
        finish_audio_player(player)


def tts_command_args(text: str, output_path: str = "") -> Tuple[List[str], bool]:
    command = env_first("NEWBOT_TTS_COMMAND", "TTS_COMMAND")
    if not command:
        raise RuntimeError("NEWBOT_TTS_COMMAND is empty")
    args = shlex.split(command, posix=(os.name != "nt"))
    uses_text_placeholder = any("{text}" in arg for arg in args)
    final_args = [
        arg.replace("{text}", text).replace("{output}", output_path)
        for arg in args
    ]
    return final_args, not uses_text_placeholder


def speak_command_tts(text: str) -> None:
    command = env_first("NEWBOT_TTS_COMMAND", "TTS_COMMAND")
    suffix = normalize_audio_suffix(
        env_first("NEWBOT_TTS_COMMAND_OUTPUT_SUFFIX", "TTS_COMMAND_OUTPUT_SUFFIX")
        or DEFAULT_TTS_COMMAND_OUTPUT_SUFFIX
    )
    cached = cached_tts_path(text, "command", suffix)
    if cached:
        if env_bool("NEWBOT_TTS_DEBUG", False):
            eprint("TTS cache:", cached)
        play_audio_file(cached)
        return

    if "{output}" in command:
        fd, output_path = tempfile.mkstemp(prefix="friday_tts_", suffix=suffix)
        os.close(fd)
        try:
            args, pass_stdin = tts_command_args(text, output_path)
            proc = subprocess.run(
                args,
                input=text.encode("utf-8") if pass_stdin else None,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if proc.returncode != 0:
                detail = proc.stderr.decode("utf-8", errors="replace").strip()
                raise RuntimeError(f"TTS command failed with code {proc.returncode}: {detail}")
            if not os.path.exists(output_path) or os.path.getsize(output_path) == 0:
                raise RuntimeError("TTS command did not create an audio file")
            cached_output = copy_tts_cache_file(text, "command", suffix, output_path)
            play_audio_file(cached_output or output_path)
        finally:
            try:
                os.unlink(output_path)
            except OSError:
                pass
        return

    args, pass_stdin = tts_command_args(text)
    proc = subprocess.run(
        args,
        input=text.encode("utf-8") if pass_stdin else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"TTS command failed with code {proc.returncode}: {detail}")
    cached_output = write_tts_cache_bytes(text, "command", suffix, proc.stdout)
    if cached_output:
        play_audio_file(cached_output)
    else:
        play_audio_bytes(proc.stdout)


def speak_text(text: str) -> None:
    spoken = strip_markdown_for_speech(text)
    if not spoken:
        return
    if not env_bool("NEWBOT_TTS_ENABLED", DEFAULT_TTS_ENABLED):
        return

    try:
        if speak_voice_clip_if_available(spoken):
            return
        backend = (env_first("NEWBOT_TTS_BACKEND", "TTS_BACKEND") or DEFAULT_TTS_BACKEND).lower()
        if backend in {"command", "custom", "local", "clone"}:
            speak_command_tts(spoken)
        elif backend == "edge":
            asyncio.run(speak_edge_tts_async(spoken))
        else:
            raise RuntimeError(f"unknown TTS backend: {backend}")
    except Exception as exc:
        eprint(f"TTS warning: {exc}")


def content_from_choice(choice: dict) -> str:
    message = choice.get("message") or {}
    if message.get("content"):
        return str(message["content"])

    delta = choice.get("delta") or {}
    if delta.get("content"):
        return str(delta["content"])

    if choice.get("content"):
        return str(choice["content"])

    return ""


def parse_stream_payload(text: str) -> str:
    payload = json.loads(text)
    choices = payload.get("choices") or []
    if not choices:
        return ""
    return content_from_choice(choices[0])


def chat_messages_with_local_llm(
    messages: List[dict],
    max_tokens: int,
    temperature: float,
    stream: bool,
    echo_stream: bool = False,
) -> Tuple[str, bool]:
    request_messages = messages_with_no_think(messages)
    body = {
        "model": local_llm_model(),
        "messages": request_messages,
        "stream": stream,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    if local_llm_disable_thinking():
        body["chat_template_kwargs"] = {"enable_thinking": False}
    headers = {"Content-Type": "application/json"}
    api_key = env_first("NEWBOT_LOCAL_LLM_API_KEY", "LOCAL_LLM_API_KEY")
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    request = urllib.request.Request(
        local_llm_url(),
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers=headers,
    )
    timeout = env_int("NEWBOT_LOCAL_LLM_TIMEOUT_SEC", DEFAULT_LOCAL_LLM_TIMEOUT_SEC)
    strip_thinking = env_bool("NEWBOT_LOCAL_LLM_STRIP_THINKING", DEFAULT_LOCAL_LLM_STRIP_THINKING)

    if stream:
        answer_parts: List[str] = []
        echo_live = False
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                for raw_line in response:
                    line = raw_line.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    if line.startswith("data:"):
                        line = line[5:].strip()
                    if not line or line == "[DONE]":
                        continue
                    if not line.startswith("{"):
                        continue
                    content = parse_stream_payload(line)
                    if content:
                        if echo_live:
                            print(content, end="", flush=True)
                        answer_parts.append(content)
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"local LLM HTTP {exc.code}: {raw}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"local LLM is not reachable at {local_llm_url()}: {exc}") from exc

        raw_answer = "".join(answer_parts).strip()
        answer = strip_thinking_text(raw_answer) if strip_thinking else raw_answer
        answer = strip_markdown_for_display(answer)
        if echo_stream:
            if echo_live:
                print("", flush=True)
            elif answer:
                print(answer, flush=True)
        return answer, echo_stream

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"local LLM HTTP {exc.code}: {raw}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"local LLM is not reachable at {local_llm_url()}: {exc}") from exc

    payload = json.loads(raw)
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError(f"unexpected local LLM response: {raw}")
    choice = choices[0]
    answer = content_from_choice(choice).strip()
    if strip_thinking:
        answer = strip_thinking_text(answer)
    answer = strip_markdown_for_display(answer)
    if not answer:
        message = choice.get("message") or {}
        reasoning = message.get("reasoning_content") or choice.get("reasoning_content")
        if reasoning:
            finish_reason = choice.get("finish_reason") or "unknown"
            raise RuntimeError(
                "empty local LLM response: model returned reasoning_content only "
                f"(finish_reason={finish_reason})"
            )
        raise RuntimeError(f"empty local LLM response: {raw[:500]}")
    return answer, False


def chat_with_local_llm(question: str) -> Optional[Tuple[str, bool]]:
    if not local_llm_enabled():
        return None

    use_stream = env_bool("NEWBOT_LOCAL_LLM_STREAM", DEFAULT_LOCAL_LLM_STREAM)
    max_tokens = env_int("NEWBOT_LOCAL_LLM_MAX_TOKENS", DEFAULT_LOCAL_LLM_MAX_TOKENS)
    temperature = env_float("NEWBOT_LOCAL_LLM_TEMPERATURE", DEFAULT_LOCAL_LLM_TEMPERATURE)
    messages = build_chat_messages(question)
    answer, already_printed = chat_messages_with_local_llm(
        messages,
        max_tokens=max_tokens,
        temperature=temperature,
        stream=use_stream,
        echo_stream=use_stream,
    )
    if already_printed:
        speak_text(answer)
    return answer, already_printed


def chat_once_with_local_llm(system_prompt: str, user_prompt: str, max_tokens: int = 80) -> str:
    if not local_llm_enabled():
        return ""
    messages = [
        {
            "role": "system",
            "content": system_prompt + " 不要输出思考过程，不要使用 <think> 标签。",
        },
        {
            "role": "user",
            "content": user_prompt,
        },
    ]
    answer, _already_printed = chat_messages_with_local_llm(
        messages,
        max_tokens=max_tokens,
        temperature=0.1,
        stream=False,
        echo_stream=False,
    )
    return answer.strip()


def chat_with_http_api(question: str) -> Optional[Tuple[str, bool]]:
    if not cloud_llm_fallback_enabled():
        return None

    api_password = cloud_llm_api_password()
    if not api_password:
        raise RuntimeError("XUNFEI_APIPASSWORD is not configured for cloud chat")

    url = env_first("XUNFEI_SPARK_URL", "SPARK_API_URL")
    if not url:
        url = DEFAULT_XUNFEI_SPARK_URL

    model = env_first("XUNFEI_SPARK_MODEL", "SPARK_MODEL")
    if not model:
        model = DEFAULT_XUNFEI_SPARK_MODEL

    use_stream = env_bool("XUNFEI_STREAM", DEFAULT_XUNFEI_STREAM)

    max_tokens = env_int("NEWBOT_CLOUD_LLM_MAX_TOKENS", DEFAULT_CLOUD_LLM_MAX_TOKENS)
    temperature = env_float("NEWBOT_CLOUD_LLM_TEMPERATURE", DEFAULT_CLOUD_LLM_TEMPERATURE)

    body = {
        "messages": build_chat_messages(question),
        "stream": use_stream,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    if model.lower() not in {"none", "auto", ""}:
        body["model"] = model

    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_password}",
        },
    )

    if use_stream:
        answer_parts: List[str] = []
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                for raw_line in response:
                    line = raw_line.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    if line.startswith("data:"):
                        line = line[5:].strip()
                    if not line or line == "[DONE]":
                        continue
                    content = parse_stream_payload(line)
                    if content:
                        answer_parts.append(content)
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {raw}") from exc

        answer = strip_markdown_for_display(strip_thinking_text("".join(answer_parts)).strip())
        if answer:
            print(answer, flush=True)
        speak_text(answer)
        return answer, True

    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {raw}") from exc

    payload = json.loads(raw)
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError(f"unexpected LLM response: {raw}")

    content = content_from_choice(choices[0])
    if content:
        answer = strip_markdown_for_display(strip_thinking_text(content).strip())
        return answer, False

    raise RuntimeError(f"empty LLM response: {raw}")


def chat_once_with_http_api(system_prompt: str, user_prompt: str, max_tokens: int = 80) -> str:
    if not cloud_llm_fallback_enabled():
        return ""

    api_password = cloud_llm_api_password()
    if not api_password:
        return ""

    url = env_first("XUNFEI_SPARK_URL", "SPARK_API_URL") or DEFAULT_XUNFEI_SPARK_URL
    model = env_first("XUNFEI_SPARK_MODEL", "SPARK_MODEL") or DEFAULT_XUNFEI_SPARK_MODEL

    body = {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "stream": False,
        "max_tokens": max_tokens,
        "temperature": 0.1,
    }
    if model.lower() not in {"none", "auto", ""}:
        body["model"] = model

    request = urllib.request.Request(
        url,
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_password}",
        },
    )

    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {raw}") from exc

    payload = json.loads(raw)
    choices = payload.get("choices") or []
    if not choices:
        return ""
    return content_from_choice(choices[0]).strip()


def fallback_song_query(text: str) -> str:
    value = text.strip()
    for token in (
        "我想听", "想听一下", "想听", "播放一下", "播放",
        "放一首", "放首", "来一首", "听一首", "听首",
        "换一首", "换首", "给我", "帮我", "歌曲", "音乐",
        "吧", "呀", "啊", "。", "，", ",",
    ):
        value = value.replace(token, "")
    return value.strip(" ：:《》\"'")


def extract_song_query(text: str) -> str:
    # Tool-only extraction: do not use Friday persona here, or the structured
    # RANDOM/NONE response can be polluted by conversational wording.
    system_prompt = (
        "你只负责从用户中文里提取点歌关键词。"
        "如果用户明确说出歌名或歌手，输出最适合音乐搜索的关键词，例如：周杰伦 晴天。"
        "如果用户只是说唱首歌/随便来一首/来点音乐，输出 RANDOM。"
        "如果不是点歌请求，输出 NONE。"
        "不要解释，不要加标点。"
    )
    try:
        result = chat_once_with_local_llm(system_prompt, text, max_tokens=32).strip()
    except Exception as exc:
        eprint(f"music query local LLM warning: {exc}")
        result = ""

    if not result:
        try:
            result = chat_once_with_http_api(system_prompt, text, max_tokens=32).strip()
        except Exception as exc:
            eprint(f"music query cloud LLM warning: {exc}")
            result = ""

    result = result.strip(" \t\r\n。。，,：:《》\"'")
    upper = result.upper()
    if upper in {"NONE", "NO", "不是点歌请求"}:
        return ""
    if upper == "RANDOM" or result in {"唱歌", "唱首歌", "随便", "随便来一首", "来点音乐"}:
        return "RANDOM"
    if result:
        return result

    fallback = fallback_song_query(text)
    if fallback in {"", "唱歌", "唱首歌", "随便", "来点音乐"}:
        return "RANDOM"
    return fallback


def random_song_query() -> str:
    candidates = [
        "周杰伦 晴天",
        "林俊杰 江南",
        "陈奕迅 十年",
        "邓紫棋 光年之外",
        "王菲 红豆",
        "孙燕姿 遇见",
        "轻音乐 钢琴",
        "放松 纯音乐",
    ]
    return random.choice(candidates)


def validate_music_url(song_url: str) -> bool:
    if not env_bool("NEWBOT_MUSIC_VALIDATE_URL", DEFAULT_MUSIC_VALIDATE_URL):
        return True
    try:
        request = urllib.request.Request(
            song_url,
            method="GET",
            headers={"User-Agent": "Mozilla/5.0"},
        )
        with urllib.request.urlopen(request, timeout=8) as response:
            final_url = response.geturl()
            return "404" not in final_url and response.status < 400
    except Exception:
        return False


def music_provider_order() -> List[str]:
    raw = (
        env_first("NEWBOT_MUSIC_PROVIDER", "XIAOMAN_MUSIC_PROVIDER", "MUSIC_PROVIDER")
        or DEFAULT_MUSIC_PROVIDER
    )
    normalized = raw.strip().lower().replace("，", ",")
    if not normalized:
        normalized = DEFAULT_MUSIC_PROVIDER

    aliases = {
        "163": "netease",
        "neteasecloud": "netease",
        "wangyi": "netease",
        "网易": "netease",
        "qqmusic": "qq",
        "qq音乐": "qq",
    }
    providers = [aliases.get(item.strip(), item.strip()) for item in normalized.split(",") if item.strip()]
    if not providers:
        providers = [DEFAULT_MUSIC_PROVIDER]
    if providers == ["auto"]:
        providers = ["qq", "netease"]

    result: List[str] = []
    for provider in providers:
        if provider and provider not in result:
            result.append(provider)
    return result


def qq_music_api_url() -> str:
    return env_first("NEWBOT_QQ_MUSIC_API_URL", "XIAOMAN_QQ_MUSIC_API_URL", "QQ_MUSIC_API_URL")


def music_resolver_request_url(base_url: str, query: str) -> str:
    if "{query}" in base_url:
        return base_url.replace("{query}", quote_plus(query))
    separator = "&" if "?" in base_url else "?"
    return f"{base_url}{separator}{urlencode({'q': query})}"


def first_dict_payload(value: Any) -> Dict[str, Any]:
    if isinstance(value, dict):
        return value
    if isinstance(value, list):
        for item in value:
            if isinstance(item, dict):
                return item
    return {}


def stringify_music_field(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, dict):
        for key in ("name", "title", "text", "value"):
            text = stringify_music_field(value.get(key))
            if text:
                return text
        return ""
    if isinstance(value, list):
        parts = [stringify_music_field(item) for item in value]
        return "/".join(part for part in parts if part)
    return str(value).strip()


def first_music_value(payload: Any, keys: Tuple[str, ...]) -> str:
    data = first_dict_payload(payload)
    for key in keys:
        text = stringify_music_field(data.get(key))
        if text:
            return text

    for nested_key in ("data", "result", "song", "music", "track", "item"):
        child = data.get(nested_key)
        if child is None:
            continue
        text = first_music_value(child, keys)
        if text:
            return text
    return ""


MUSIC_URL_KEYS = (
    "url",
    "play_url",
    "playUrl",
    "song_url",
    "songUrl",
    "music_url",
    "musicUrl",
    "audio",
    "src",
    "purl",
    "vkeyUrl",
)
MUSIC_TITLE_KEYS = ("title", "name", "song", "song_name", "songName", "songname")
MUSIC_ARTIST_KEYS = (
    "artist",
    "artists",
    "singer",
    "singers",
    "author",
    "subtitle",
    "ar",
)
MUSIC_COVER_KEYS = (
    "cover",
    "cover_url",
    "coverUrl",
    "pic",
    "pic_url",
    "picUrl",
    "albumPic",
    "album_pic",
    "image",
)
MUSIC_ID_KEYS = ("id", "song_id", "songid", "songId", "songmid", "mid", "media_mid", "strMediaMid")


def collect_music_items(payload: Any) -> List[Dict[str, Any]]:
    if isinstance(payload, list):
        return [item for item in payload if isinstance(item, dict)]
    if not isinstance(payload, dict):
        return []

    for key in (
        "list",
        "songs",
        "items",
        "tracks",
        "data",
        "result",
        "song",
        "music",
        "records",
    ):
        child = payload.get(key)
        if isinstance(child, list):
            return [item for item in child if isinstance(item, dict)]
        if isinstance(child, dict):
            nested = collect_music_items(child)
            if nested:
                return nested
    return [payload]


def first_url_like(payload: Any) -> str:
    if isinstance(payload, str):
        text = payload.strip()
        if text.startswith(("http://", "https://")):
            return text
        return ""
    if isinstance(payload, dict):
        for value in payload.values():
            text = first_url_like(value)
            if text:
                return text
    if isinstance(payload, list):
        for value in payload:
            text = first_url_like(value)
            if text:
                return text
    return ""


def normalize_music_url(song_url: str) -> str:
    text = (song_url or "").strip()
    if not text:
        return ""
    if text.startswith("//"):
        return "https:" + text
    if text.startswith(("http://", "https://")):
        return text
    if text.startswith("/"):
        text = text.lstrip("/")
    if re.search(r"\.(m4a|mp3|flac|aac)(\?|$)", text, re.IGNORECASE):
        return "http://ws.stream.qqmusic.qq.com/" + text
    return text


def music_candidate_from_payload(item: Dict[str, Any], query: str) -> Dict[str, str]:
    candidate: Dict[str, str] = {
        "title": first_music_value(item, MUSIC_TITLE_KEYS) or query,
        "artist": first_music_value(item, MUSIC_ARTIST_KEYS),
        "url": normalize_music_url(first_music_value(item, MUSIC_URL_KEYS)),
        "cover": first_music_value(item, MUSIC_COVER_KEYS),
    }
    for key in MUSIC_ID_KEYS:
        value = first_music_value(item, (key,))
        if value:
            normalized_key = {
                "song_id": "songid",
                "songId": "songid",
                "strMediaMid": "media_mid",
            }.get(key, key)
            candidate[normalized_key] = value
    if not candidate["url"]:
        candidate["url"] = normalize_music_url(first_url_like(item))
    return candidate


def search_qq_music_candidates(query: str, limit: int) -> List[Dict[str, str]]:
    title, artist, url = search_qq_music(query)
    return [{"title": title, "artist": artist, "url": url, "cover": ""}]


def search_qq_music(query: str) -> Tuple[str, str, str]:
    base_url = qq_music_api_url()
    if not base_url:
        raise RuntimeError("QQ music resolver is not configured")

    request_url = music_resolver_request_url(base_url, query)
    headers = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}
    token = env_first("NEWBOT_QQ_MUSIC_API_TOKEN", "QQ_MUSIC_API_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(request_url, headers=headers)
    with urllib.request.urlopen(request, timeout=12) as response:
        raw = response.read().decode("utf-8", errors="replace")

    payload = json.loads(raw)
    song_url = first_music_value(
        payload,
        ("url", "play_url", "playUrl", "song_url", "songUrl", "music_url", "musicUrl", "audio", "src"),
    )
    title = first_music_value(payload, ("title", "name", "song", "song_name", "songName")) or query
    artist = first_music_value(payload, ("artist", "artists", "singer", "singers", "author", "subtitle"))
    if not song_url:
        raise RuntimeError("qq music resolver returned no playable url")
    if not validate_music_url(song_url):
        raise RuntimeError("qq music url is not playable")
    return title, artist, song_url


def search_netease_music_candidates(query: str, limit: int) -> List[Dict[str, str]]:
    params = urlencode({"s": query, "type": 1, "offset": 0, "limit": max(5, limit)})
    request = urllib.request.Request(
        f"https://music.163.com/api/search/get/web?{params}",
        headers={
            "User-Agent": "Mozilla/5.0",
            "Referer": "https://music.163.com/",
        },
    )
    with urllib.request.urlopen(request, timeout=12) as response:
        payload = json.loads(response.read().decode("utf-8", errors="replace"))

    songs = ((payload.get("result") or {}).get("songs") or [])
    candidates: List[Dict[str, str]] = []
    for song in songs:
        song_id = song.get("id")
        if not song_id:
            continue
        song_url = f"https://music.163.com/song/media/outer/url?id={song_id}.mp3"
        if not validate_music_url(song_url):
            continue
        artists = song.get("artists") or []
        artist_text = "/".join(str(item.get("name", "")) for item in artists if item.get("name"))
        candidates.append(
            {
                "title": str(song.get("name", query)),
                "artist": artist_text,
                "url": song_url,
                "cover": "",
            }
        )
        if len(candidates) >= limit:
            break

    if not candidates:
        raise RuntimeError(f"music not found: {query}")
    return candidates


def search_netease_music(query: str) -> Tuple[str, str, str]:
    candidate = search_netease_music_candidates(query, 1)[0]
    return candidate.get("title", query), candidate.get("artist", ""), candidate["url"]


def search_music_candidates(query: str, limit: Optional[int] = None) -> List[Dict[str, str]]:
    title, artist, url = search_music(query)
    return [{"title": title, "artist": artist, "url": url, "cover": ""}]


def search_music(query: str) -> Tuple[str, str, str]:
    errors: List[str] = []
    for provider in music_provider_order():
        try:
            if provider == "netease":
                return search_netease_music(query)
            if provider == "qq":
                return search_qq_music(query)
            errors.append(f"{provider}: unsupported provider")
        except Exception as exc:
            errors.append(f"{provider}: {exc}")
            eprint(f"music provider {provider} warning: {exc}")
    raise RuntimeError("; ".join(errors) if errors else f"music not found: {query}")


def print_music_kv(key: str, value: str) -> None:
    safe_value = (value or "").replace("\r", " ").replace("\n", " ").strip()
    print(f"{key}={safe_value}")


def print_music_url_for_request(text: str) -> int:
    query = extract_song_query(text)
    if not query:
        print("MUSIC_ERROR=not a music request")
        return 1
    if query == "RANDOM":
        query = random_song_query()

    try:
        title, artist, url = search_music(query)
    except Exception as exc:
        print(f"MUSIC_ERROR={exc}")
        return 1

    print_music_kv("MUSIC_TITLE", title)
    print_music_kv("MUSIC_ARTIST", artist)
    print_music_kv("MUSIC_URL", url)
    return 0


def parse_helper_request(raw: str) -> Tuple[str, str]:
    """Return (question, jpeg_base64).  Raw text keeps the old chat path."""
    raw = raw.strip()
    if not raw:
        return "", ""

    if raw.startswith("{"):
        try:
            payload = json.loads(raw)
            question = str(payload.get("text") or payload.get("question") or "").strip()
            image_b64 = str(payload.get("image_b64") or payload.get("image") or "").strip()
            if image_b64.startswith("data:image"):
                image_b64 = image_b64.split(",", 1)[-1]
            return question, image_b64
        except json.JSONDecodeError:
            pass

    return raw, ""


def create_xunfei_ws_url(api_key: str, api_secret: str, service_url: str) -> str:
    parsed = urlparse(service_url)
    host = parsed.netloc
    path = parsed.path or "/"
    date = format_date_time(mktime(datetime.now().timetuple()))

    signature_origin = f"host: {host}\n"
    signature_origin += f"date: {date}\n"
    signature_origin += f"GET {path} HTTP/1.1"
    signature = hmac.new(
        api_secret.encode("utf-8"),
        signature_origin.encode("utf-8"),
        digestmod=hashlib.sha256,
    ).digest()
    signature_b64 = base64.b64encode(signature).decode("utf-8")

    LAST_VISION_AUTH_DEBUG.host = host
    LAST_VISION_AUTH_DEBUG.path = path
    LAST_VISION_AUTH_DEBUG.date = date
    LAST_VISION_AUTH_DEBUG.signature_len = len(signature_b64)

    authorization_origin = (
        f'api_key="{api_key}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{signature_b64}"'
    )
    authorization = base64.b64encode(authorization_origin.encode("utf-8")).decode("utf-8")
    return service_url + "?" + urlencode(
        {
            "authorization": authorization,
            "date": date,
            "host": host,
        }
    )


def vision_auth_url_debug() -> int:
    appid, appid_source = config_value(
        "XUNFEI_VISION_APPID", DEFAULT_XUNFEI_VISION_APPID, "XUNFEI_APPID"
    )
    api_key, api_key_source = config_value(
        "XUNFEI_VISION_APIKEY", DEFAULT_XUNFEI_VISION_APIKEY, "XUNFEI_APIKEY"
    )
    api_secret, api_secret_source = config_value(
        "XUNFEI_VISION_APISECRET", DEFAULT_XUNFEI_VISION_APISECRET, "XUNFEI_APISECRET"
    )
    service_url = env_first("XUNFEI_VISION_URL") or DEFAULT_XUNFEI_VISION_URL
    ws_url = create_xunfei_ws_url(api_key, api_secret, service_url)
    parsed = urlparse(ws_url)
    query = parse_qs(parsed.query, keep_blank_values=True)
    authorization = (query.get("authorization") or [""])[0]
    decoded_auth = ""
    try:
        decoded_auth = base64.b64decode(authorization.encode("utf-8")).decode("utf-8")
    except Exception:
        decoded_auth = "<decode failed>"

    signature_len = 0
    if 'signature="' in decoded_auth:
        signature = decoded_auth.split('signature="', 1)[1].split('"', 1)[0]
        signature_len = len(signature)

    print(f"service_url={service_url}")
    print(f"url_scheme={parsed.scheme}")
    print(f"url_host={parsed.netloc}")
    print(f"url_path={parsed.path}")
    print(f"url_length={len(ws_url)}")
    print(f"query_keys={','.join(sorted(query.keys()))}")
    print(f"authorization_present={int(bool(authorization))}")
    print(f"authorization_len={len(authorization)}")
    print(f"date={query.get('date', [''])[0]}")
    print(f"host={query.get('host', [''])[0]}")
    print(f"appid={mask_secret(appid)}({appid_source})")
    print(f"api_key={mask_secret(api_key)}({api_key_source})")
    print(f"api_secret={mask_secret(api_secret)}({api_secret_source})")
    print(f"decoded_auth_prefix={decoded_auth[:80]}")
    print(f"decoded_signature_len={signature_len}")
    print(f"signature_origin_host={LAST_VISION_AUTH_DEBUG.host}")
    print(f"signature_origin_path={LAST_VISION_AUTH_DEBUG.path}")
    print(f"signature_origin_date={LAST_VISION_AUTH_DEBUG.date}")
    print(f"signature_origin_sha256={hashlib.sha256((LAST_VISION_AUTH_DEBUG.host + '|' + LAST_VISION_AUTH_DEBUG.path + '|' + LAST_VISION_AUTH_DEBUG.date).encode('utf-8')).hexdigest()[:16]}")
    return 0


def websocket_error_detail(exc: Exception) -> str:
    details: List[str] = []
    for attr in ("status_code", "status", "resp_status"):
        value = getattr(exc, attr, None)
        if value:
            details.append(f"{attr}={value}")

    headers = getattr(exc, "resp_headers", None) or getattr(exc, "headers", None)
    if headers:
        if isinstance(headers, dict):
            interesting = []
            for key in ("date", "content-type", "x-request-id"):
                for raw_key, raw_value in headers.items():
                    if str(raw_key).lower() == key:
                        interesting.append(f"{raw_key}: {raw_value}")
            if interesting:
                details.append("headers={" + "; ".join(interesting) + "}")
            else:
                details.append(f"headers={headers}")
        else:
            details.append(f"headers={headers}")

    body = getattr(exc, "resp_body", None) or getattr(exc, "response", None)
    if body:
        if isinstance(body, bytes):
            body = body.decode("utf-8", errors="replace")
        details.append(f"body={str(body).strip()}")

    return "; ".join(details)


def run_vision_websocket_app(websocket_module,
                             ws_url: str,
                             body: Optional[dict],
                             echo_output: bool = True) -> List[str]:
    answer_parts: List[str] = []
    errors: List[str] = []
    opened = threading.Event()
    closed = threading.Event()

    def is_close_race_error(error_text: str) -> bool:
        return (
            "NoneType" in error_text
            and ("connected" in error_text or "sock" in error_text)
        )

    def on_open(ws):
        opened.set()
        if body is None:
            ws.close()
            return
        ws.send(json.dumps(body, ensure_ascii=False))

    def on_message(ws, message):
        payload = json.loads(message)
        header = payload.get("header") or {}
        code = int(header.get("code", 0))
        if code != 0:
            errors.append(f"vision API error {code}: {payload}")
            ws.close()
            return

        choices = ((payload.get("payload") or {}).get("choices") or {})
        for item in choices.get("text") or []:
            content = str(item.get("content") or "")
            if content:
                if echo_output:
                    print(content, end="", flush=True)
                answer_parts.append(content)

        if int(choices.get("status", header.get("status", 0))) == 2:
            ws.close()

    def on_error(_ws, error):
        error_text = str(error)
        if answer_parts and is_close_race_error(error_text):
            return
        detail = websocket_error_detail(error)
        if detail:
            errors.append(f"{error}. detail: {detail}")
        else:
            errors.append(error_text)

    def on_close(_ws, _status_code, _message):
        closed.set()

    ws = websocket_module.WebSocketApp(
        ws_url,
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
    )

    timeout = threading.Timer(45.0, ws.close)
    timeout.daemon = True
    timeout.start()
    try:
        ws.run_forever(sslopt={"cert_reqs": ssl.CERT_NONE})
    finally:
        timeout.cancel()

    if errors and not answer_parts:
        raise RuntimeError(errors[-1])
    if body is not None and not answer_parts:
        if not opened.is_set():
            raise RuntimeError("websocket was not opened")
        if not closed.is_set():
            raise RuntimeError("websocket closed without response")
    return answer_parts


def build_vision_request_body(appid: str, domain: str, image_b64: str, prompt: str) -> dict:
    return {
        "header": {
            "app_id": appid,
        },
        "parameter": {
            "chat": {
                "domain": domain,
                "temperature": 0.5,
                "top_k": 4,
                "max_tokens": 1024,
                "auditing": "default",
            }
        },
        "payload": {
            "message": {
                "text": [
                    {
                        "role": "user",
                        "content": image_b64,
                        "content_type": "image",
                    },
                    {
                        "role": "user",
                        "content": prompt,
                        "content_type": "text",
                    },
                ]
            }
        },
    }


def call_vision_api(websocket_module,
                    ws_url: str,
                    appid: str,
                    domain: str,
                    image_b64: str,
                    prompt: str,
                    echo_output: bool) -> str:
    body = build_vision_request_body(appid, domain, image_b64, prompt)
    answer_parts = run_vision_websocket_app(websocket_module, ws_url, body, echo_output=echo_output)
    return "".join(answer_parts).strip()


def image2text_with_websocket(image_b64: str, question: str) -> Tuple[str, bool]:
    appid, appid_source = config_value(
        "XUNFEI_VISION_APPID", DEFAULT_XUNFEI_VISION_APPID, "XUNFEI_APPID"
    )
    api_key, api_key_source = config_value(
        "XUNFEI_VISION_APIKEY", DEFAULT_XUNFEI_VISION_APIKEY, "XUNFEI_APIKEY"
    )
    api_secret, api_secret_source = config_value(
        "XUNFEI_VISION_APISECRET", DEFAULT_XUNFEI_VISION_APISECRET, "XUNFEI_APISECRET"
    )
    if not appid or not api_key or not api_secret:
        raise RuntimeError(
            "vision API is not configured. Set XUNFEI_APPID, "
            "XUNFEI_APIKEY and XUNFEI_APISECRET, or fill the "
            "DEFAULT_XUNFEI_VISION_* constants in this file."
        )

    try:
        import websocket  # pylint: disable=import-error,import-outside-toplevel
    except Exception as exc:
        raise RuntimeError(
            "websocket-client is not installed. Install: "
            "sudo python3 -m pip install websocket-client"
        ) from exc

    service_url = env_first("XUNFEI_VISION_URL") or DEFAULT_XUNFEI_VISION_URL
    domain = env_first("XUNFEI_VISION_DOMAIN") or DEFAULT_XUNFEI_VISION_DOMAIN
    prompt = build_vision_prompt(question)
    if not image_b64.strip():
        raise RuntimeError("empty image payload for vision API")

    ws_url = create_xunfei_ws_url(api_key, api_secret, service_url)
    refine_answer = vision_refine_enabled()
    try:
        if vision_debug_enabled():
            eprint(
                "vision request: "
                f"image_b64_len={len(image_b64)}, prompt_len={len(prompt)}, "
                f"domain={domain}, refine={int(refine_answer)}"
            )
        raw_answer = call_vision_api(
            websocket,
            ws_url,
            appid,
            domain,
            image_b64,
            prompt,
            echo_output=False,
        )
        if is_missing_image_vision_answer(raw_answer):
            retry_prompt = question.strip() or "请描述这张图片里的主要内容，用一句中文回答。"
            if retry_prompt == prompt:
                retry_prompt = "请描述这张图片里的主要内容，用一句中文回答。"
            eprint("vision warning: API returned a no-image answer; retrying with a minimal prompt")
            raw_answer = call_vision_api(
                websocket,
                ws_url,
                appid,
                domain,
                image_b64,
                retry_prompt,
                echo_output=False,
            )
    except Exception as exc:
        parsed = urlparse(service_url)
        detail = websocket_error_detail(exc)
        if detail:
            detail = " detail: " + detail + "."
        raise RuntimeError(
            "vision websocket handshake failed: "
            f"{exc}.{detail} config appid={mask_secret(appid)}({appid_source}), "
            f"api_key={mask_secret(api_key)}({api_key_source}), "
            f"api_secret={mask_secret(api_secret)}({api_secret_source}), "
            f"host={parsed.netloc}, path={parsed.path or '/'}, "
            f"date={LAST_VISION_AUTH_DEBUG.date}, "
            f"signature_len={LAST_VISION_AUTH_DEBUG.signature_len}. "
            "If this is 401, check whether the APPID/APIKey/APISecret belong to "
            "the same image-understanding WebSocket app, and verify the Orange Pi "
            "system time is correct."
        ) from exc

    if not raw_answer:
        raise RuntimeError("empty vision API response")
    if is_missing_image_vision_answer(raw_answer):
        raise RuntimeError(
            "vision API returned a text-only/no-image answer after retry "
            f"(image_b64_len={len(image_b64)}, domain={domain}). "
            "Check XUNFEI_VISION_URL, XUNFEI_VISION_DOMAIN, and image API credentials."
        )
    answer = strip_markdown_for_display(refine_vision_answer(question, raw_answer))
    print(answer, flush=True)
    speak_text(answer)
    append_chat_memory(question.strip() or "你看到了什么？", answer)
    return answer, True


def vision_auth_test() -> int:
    appid, appid_source = config_value(
        "XUNFEI_VISION_APPID", DEFAULT_XUNFEI_VISION_APPID, "XUNFEI_APPID"
    )
    api_key, api_key_source = config_value(
        "XUNFEI_VISION_APIKEY", DEFAULT_XUNFEI_VISION_APIKEY, "XUNFEI_APIKEY"
    )
    api_secret, api_secret_source = config_value(
        "XUNFEI_VISION_APISECRET", DEFAULT_XUNFEI_VISION_APISECRET, "XUNFEI_APISECRET"
    )
    service_url = env_first("XUNFEI_VISION_URL") or DEFAULT_XUNFEI_VISION_URL

    if not appid or not api_key or not api_secret:
        raise RuntimeError("vision API is not configured")

    try:
        import websocket  # pylint: disable=import-error,import-outside-toplevel
    except Exception as exc:
        raise RuntimeError(
            "websocket-client is not installed. Install: "
            "sudo python3 -m pip install websocket-client"
        ) from exc

    ws_url = create_xunfei_ws_url(api_key, api_secret, service_url)
    try:
        ws = websocket.create_connection(
            ws_url,
            timeout=15,
            sslopt={"cert_reqs": ssl.CERT_NONE},
        )
        ws.close()
    except Exception as exc:
        detail = websocket_error_detail(exc)
        if detail:
            detail = " detail: " + detail + "."
        raise RuntimeError(
            "vision auth test failed: "
            f"{exc}.{detail} appid={mask_secret(appid)}({appid_source}), "
            f"api_key={mask_secret(api_key)}({api_key_source}), "
            f"api_secret={mask_secret(api_secret)}({api_secret_source}), "
            f"host={LAST_VISION_AUTH_DEBUG.host}, path={LAST_VISION_AUTH_DEBUG.path}, "
            f"date={LAST_VISION_AUTH_DEBUG.date}, "
            f"signature_len={LAST_VISION_AUTH_DEBUG.signature_len}"
        ) from exc

    print(
        "vision auth ok: "
        f"appid={mask_secret(appid)}({appid_source}), "
        f"api_key={mask_secret(api_key)}({api_key_source}), "
        f"host={LAST_VISION_AUTH_DEBUG.host}, "
        f"path={LAST_VISION_AUTH_DEBUG.path}, "
        f"date={LAST_VISION_AUTH_DEBUG.date}, "
        f"signature_len={LAST_VISION_AUTH_DEBUG.signature_len}"
    )
    return 0


def main() -> int:
    args = sys.argv[1:]
    if "--vision-auth-url-debug" in args:
        return vision_auth_url_debug()
    if "--vision-auth-test" in args:
        return vision_auth_test()

    raw_stdin = sys.stdin.read()
    if "--music-url" in args:
        return print_music_url_for_request(raw_stdin.strip())
    if "--tts-test" in args:
        speak_text(raw_stdin.strip() or "你好，我是星期五。")
        return 0
    if "--local-llm-test" in args:
        if not local_llm_enabled():
            raise RuntimeError("local LLM is disabled by NEWBOT_LOCAL_LLM_ENABLED=0")
        answer = chat_once_with_local_llm(
            friday_system_prompt(),
            raw_stdin.strip() or "你好，请简单介绍一下你自己。",
            max_tokens=128,
        )
        print(answer)
        return 0

    question, image_b64 = parse_helper_request(raw_stdin)
    if not question:
        question = "请用一句话描述你看到的内容。" if image_b64 else ""
    if not question and not image_b64:
        print("我没有听清楚，你可以再说一遍。")
        return 0

    if image_b64:
        image2text_with_websocket(image_b64, question)
        return 0

    if is_memory_reset_request(question):
        clear_chat_memory()
        answer = "我已经清空短期记忆。"
        print(answer)
        speak_text(answer)
        return 0

    def handle_chat_result(result: Optional[Tuple[str, bool]]) -> bool:
        if result is None:
            return False
        answer, already_printed = result
        answer = strip_markdown_for_display(answer)
        if not already_printed:
            print(answer)
            speak_text(answer)
        append_chat_memory(question, answer)
        return True

    def try_chat_backend(name: str) -> bool:
        try:
            if name == "cloud":
                return handle_chat_result(chat_with_http_api(question))
            return handle_chat_result(chat_with_local_llm(question))
        except Exception as exc:
            eprint(f"{name} LLM warning: {exc}")
            return False

    backend = chat_backend()
    if backend == "cloud":
        order = ("cloud", "local")
    elif backend == "local":
        order = ("local", "cloud")
    else:
        order = ("local", "cloud")

    for name in order:
        if try_chat_backend(name):
            return 0

    if backend == "cloud" and not cloud_llm_api_password():
        answer = "云端对话密钥还没配置。本地模型也没启动，所以我现在没法回答。先把 XUNFEI_APIPASSWORD 填上。"
    else:
        answer = "我现在连不上语言模型。请检查云端 API 配置，或者启动本地 llama-server。"
    print(answer)
    speak_text(answer)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # Keep C++ side simple: errors are terminal text.
        print(f"LLM helper error: {exc}")
        raise SystemExit(1)
