#!/bin/bash
# [AIMV] One-click server setup script
# Usage: bash setup.sh [--dev] [--gpu]
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
    fastapi uvicorn openai pydantic

# Optional: Anthropic backend
ANTHROPIC_SPECIFIED=false
if $PYTHON -c "import anthropic" 2>/dev/null; then
    ANTHROPIC_SPECIFIED=true
fi

# ── Generate .env template ──
echo "[4/4] Generating configuration..."

ENV_FILE="$SCRIPT_DIR/.env"
if [ ! -f "$ENV_FILE" ]; then
    cat > "$ENV_FILE" << 'ENVEOF'
# AIMV Server Configuration
# Copy to .env and fill in your keys.
# Source with: source .env

# LLM Backend: openai | anthropic | mock
AIMV_LLM_BACKEND=mock

# Model name
AIMV_LLM_MODEL=gpt-4o

# API Key (set ONE based on backend)
OPENAI_API_KEY=
ANTHROPIC_API_KEY=
DEEPSEEK_API_KEY=

# Custom base URL (leave empty for default)
# DeepSeek OpenAI:  https://api.deepseek.com/v1
# DeepSeek Anthropic: https://api.deepseek.com/anthropic
# vLLM/Ollama:       http://localhost:8000/v1
AIMV_LLM_BASE_URL=

# MCP Server auth (optional, leave empty to disable)
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
echo "Starting AIMV Server..."
echo "  Backend: ${AIMV_LLM_BACKEND:-mock}"
echo "  Model:   ${AIMV_LLM_MODEL:-gpt-4o}"
echo "  URL:     http://localhost:${AIMV_PORT:-8080}"
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
echo "  # DeepSeek (OpenAI endpoint)"
echo "  AIMV_LLM_BACKEND=openai \\"
echo "    AIMV_LLM_BASE_URL=https://api.deepseek.com/v1 \\"
echo "    OPENAI_API_KEY=sk-... \\"
echo "    bash aimv/start_server.sh"
echo ""
echo "  # Mock mode (no API key)"
echo "  AIMV_LLM_BACKEND=mock bash aimv/start_server.sh"
echo ""
