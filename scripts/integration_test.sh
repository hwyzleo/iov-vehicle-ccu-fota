#!/bin/bash
#
# FOTA-DIAG 集成测试脚本
# 用于验证 FOTA 服务与 DIAG Mock 服务的完整业务流程
#

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 项目根目录
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
SCRIPTS_DIR="${PROJECT_ROOT}/scripts"

# 日志文件
MOCK_LOG="${PROJECT_ROOT}/mock_diag.log"
FOTA_LOG="${PROJECT_ROOT}/fota_output.log"

# 进程 ID
MOCK_PID=""
FOTA_PID=""

# 打印带颜色的消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 清理函数
cleanup() {
    print_info "Cleaning up..."
    
    if [ -n "$FOTA_PID" ]; then
        kill $FOTA_PID 2>/dev/null || true
        wait $FOTA_PID 2>/dev/null || true
    fi
    
    if [ -n "$MOCK_PID" ]; then
        kill $MOCK_PID 2>/dev/null || true
        wait $MOCK_PID 2>/dev/null || true
    fi
    
    # 清理端口占用
    lsof -ti :30501 | xargs kill -9 2>/dev/null || true
    lsof -ti :30502 | xargs kill -9 2>/dev/null || true
    
    print_info "Cleanup completed"
}

# 注册清理函数
trap cleanup EXIT INT TERM

# 检查依赖
check_dependencies() {
    print_info "Checking dependencies..."
    
    if [ ! -f "${BUILD_DIR}/ccu-fota" ]; then
        print_error "FOTA executable not found. Please build first: ./scripts/build.sh"
        exit 1
    fi
    
    if [ ! -f "${SCRIPTS_DIR}/mock_diag_service.py" ]; then
        print_error "Mock DIAG service not found"
        exit 1
    fi
    
    if ! command -v python3 &> /dev/null; then
        print_error "Python3 not found"
        exit 1
    fi
    
    print_success "Dependencies checked"
}

# 清理日志
clean_logs() {
    rm -f "$MOCK_LOG" "$FOTA_LOG"
}

# 启动 Mock DIAG 服务
start_mock_diag() {
    print_info "Starting Mock DIAG Service..."
    
    python3 "${SCRIPTS_DIR}/mock_diag_service.py" > "$MOCK_LOG" 2>&1 &
    MOCK_PID=$!
    
    # 等待服务启动
    sleep 2
    
    if ! kill -0 $MOCK_PID 2>/dev/null; then
        print_error "Failed to start Mock DIAG Service"
        cat "$MOCK_LOG"
        exit 1
    fi
    
    # 检查端口是否监听
    if ! lsof -i :30501 > /dev/null 2>&1; then
        print_error "Mock DIAG Service not listening on port 30501"
        exit 1
    fi
    
    print_success "Mock DIAG Service started (PID: $MOCK_PID)"
}

# 启动 FOTA 服务
start_fota() {
    print_info "Starting FOTA Service..."
    
    # CCU-FOTA-DSN-CR-004: ccu-fota 接入 ccu-framework-config，argv[1] 为 config 根
    # （需含 common.yaml）；传入仓库 config/ 开发夹具根。
    "${BUILD_DIR}/ccu-fota" "${PROJECT_ROOT}/config" > "$FOTA_LOG" 2>&1 &
    FOTA_PID=$!
    
    # 等待服务启动并完成初始报告
    sleep 5
    
    if ! kill -0 $FOTA_PID 2>/dev/null; then
        print_error "FOTA Service exited unexpectedly"
        cat "$FOTA_LOG"
        exit 1
    fi
    
    print_success "FOTA Service started (PID: $FOTA_PID)"
}

# 验证测试结果
verify_results() {
    print_info "Verifying test results..."
    
    local success=true
    
    # 检查 Mock DIAG 日志
    if grep -q "READ_VIN request" "$MOCK_LOG"; then
        print_success "Mock DIAG received READ_VIN request"
    else
        print_error "Mock DIAG did not receive READ_VIN request"
        success=false
    fi
    
    # 检查 FOTA 日志
    if grep -q "Initial inventory report successful" "$FOTA_LOG"; then
        print_success "FOTA initial inventory report successful"
    else
        print_error "FOTA initial inventory report failed"
        success=false
    fi
    
    if grep -q "VIN: 12345678901234567" "$FOTA_LOG"; then
        print_success "FOTA received correct VIN"
    else
        print_error "FOTA did not receive correct VIN"
        success=false
    fi
    
    if [ "$success" = true ]; then
        print_success "All integration tests passed!"
        return 0
    else
        print_error "Some integration tests failed"
        return 1
    fi
}

# 显示日志摘要
show_logs() {
    echo ""
    echo "=========================================="
    echo "Mock DIAG Service Log"
    echo "=========================================="
    cat "$MOCK_LOG"
    
    echo ""
    echo "=========================================="
    echo "FOTA Service Log"
    echo "=========================================="
    cat "$FOTA_LOG"
}

# 主函数
main() {
    echo "=========================================="
    echo "FOTA-DIAG Integration Test"
    echo "=========================================="
    echo ""
    
    # 解析参数
    local show_logs_flag=false
    while [[ $# -gt 0 ]]; do
        case $1 in
            --show-logs)
                show_logs_flag=true
                shift
                ;;
            --help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --show-logs    Show detailed logs after test"
                echo "  --help         Show this help message"
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    # 检查依赖
    check_dependencies
    
    # 清理日志
    clean_logs
    
    # 启动服务
    start_mock_diag
    start_fota
    
    # 验证结果
    if verify_results; then
        if [ "$show_logs_flag" = true ]; then
            show_logs
        fi
        print_success "Integration test completed successfully!"
        exit 0
    else
        show_logs
        print_error "Integration test failed!"
        exit 1
    fi
}

# 运行主函数
main "$@"
