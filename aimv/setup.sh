#!/bin/bash
# [AIMV] One-click server setup script
# Usage: bash setup.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "  AIMV Server — One-Click Setup"
echo "========================================"
echo ""

# ── Detect Python ──
PYTHON=""
for py in python3 python; do
    if command -v "$py" &>/dev/null; then
        ver=$("$py" --version 2>&1 | grep -oP '\d+\.\d+')
        if [ "$(echo "$ver >= 3.10" | bc -l 2>/dev/null || echo 0)" = "1" ] || [ "${ver%%.*}" -ge 3 -a "${ver#*.}" -ge 10 ] 2>/dev/null; then
            PYTHON="$py"
            break
        fi
    fi
done
if [ -z "$PYTHON" ]; then
    echo "Error: Python 3.10+ required. Install with: sudo apt install python3"
    exit 1
fi
echo "[1/4] Python: $PYTHON ($($PYTHON --version))"

# ── Install pip if missing ──
if ! $PYTHON -m pip --version &>/dev/null; then
    echo "      Installing pip..."
    sudo apt-get update -qq && sudo apt-get install -y -qq python3-pip
fi

# ── Install driver dependencies ──
echo "[2/4] Installing aimv-driver dependencies..."
$PYTHON -m pip install --break-system-packages -q \
    httpx pyyaml jinja2

# ── Install MCP server dependencies ──
echo "[3/4] Installing aimv-server dependencies..."
$PYTHON -m pip install --break-system-packages -q \
    fastapi uvicorn pydantic

# OpenAI backend (optional)
$PYTHON -m pip install --break-system-packages -q openai 2>/dev/null || true

# Anthropic backend (optional)
$PYTHON -m pip install --break-system-packages -q anthropic 2>/dev/null || true

# ── Generate .env template ──
echo "[4/4] Generating configuration..."

ENV_FILE="$SCRIPT_DIR/.env"
if [ ! -f "$ENV_FILE" ]; then
    cat > "$ENV_FILE" << 'ENVEOF'
# AIMV Server Configuration
# Choose ONE backend and fill in all three fields.

# Backend: openai | anthropic | mock
AIMV_LLM_BACKEND=mock

# ─── OpenAI backend (required if AIMV_LLM_BACKEND=openai) ───
OPENAI_API_KEY=
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-4o

# ─── Anthropic backend (required if AIMV_LLM_BACKEND=anthropic) ───
ANTHROPIC_API_KEY=
ANTHROPIC_BASE_URL=https://open.bigmodel.cn/api/anthropic
ANTHROPIC_MODEL=glm-5.1

# ─── Server settings ───
# Bearer token auth (optional, leave empty to disable)
AIMV_API_KEY=

# Cache TTL in seconds (default: 86400 = 24h)
AIMV_CACHE_TTL=86400
ENVEOF
    echo "      Created .env template at $ENV_FILE"
else
    echo "      .env already exists, skipping"
fi

# ── Create start script ──
cat > "$SCRIPT_DIR/start_server.sh" << 'STARTEOF'
#!/bin/bash
# AIMV Server launcher
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Load .env if exists
if [ -f "$SCRIPT_DIR/.env" ]; then
    set -a; source "$SCRIPT_DIR/.env"; set +a
fi

cd "$SCRIPT_DIR"

BACKEND="${AIMV_LLM_BACKEND:-mock}"
echo "Starting AIMV Server..."
echo "  Backend: $BACKEND"

case "$BACKEND" in
    openai)
        echo "  Model:   ${OPENAI_MODEL:-(not set)}"
        echo "  URL:     ${OPENAI_BASE_URL:-(not set)}"
        ;;
    anthropic)
        echo "  Model:   ${ANTHROPIC_MODEL:-(not set)}"
        echo "  URL:     ${ANTHROPIC_BASE_URL:-(not set)}"
        ;;
    mock)
        echo "  Model:   mock (offline)"
        ;;
esac
echo "  Listen:  http://localhost:${AIMV_PORT:-8080}"
echo ""

python3 -m uvicorn mcp_server.aimv_server:app \
    --host 0.0.0.0 --port "${AIMV_PORT:-8080}" \
    ${AIMV_RELOAD:+--reload}
STARTEOF
chmod +x "$SCRIPT_DIR/start_server.sh"

echo ""
echo "========================================"
echo "  Setup Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Edit .env with your API keys:"
echo "     vim aimv/.env"
echo ""
echo "  2. Start the server:"
echo "     bash aimv/start_server.sh"
echo ""
echo "  3. Test:"
echo "     curl http://localhost:8080/api/v1/health"
echo ""
echo "Quick start examples:"
echo ""
echo "  # Anthropic backend"
echo "  ANTHROPIC_API_KEY=sk-... \\"
echo "    ANTHROPIC_BASE_URL=https://open.bigmodel.cn/api/anthropic \\"
echo "    ANTHROPIC_MODEL=glm-5.1 \\"
echo "    AIMV_LLM_BACKEND=anthropic \\"
echo "    bash aimv/start_server.sh"
echo ""
echo "  # OpenAI backend"
echo "  OPENAI_API_KEY=sk-... \\"
echo "    OPENAI_BASE_URL=https://api.openai.com/v1 \\"
echo "    OPENAI_MODEL=gpt-4o \\"
echo "    AIMV_LLM_BACKEND=openai \\"
echo "    bash aimv/start_server.sh"
echo ""
echo "  # Mock mode (offline testing)"
echo "  AIMV_LLM_BACKEND=mock bash aimv/start_server.sh"
echo ""
