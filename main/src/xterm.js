/**
 * Lightweight Embedded VT100/ANSI Terminal Emulator for M5StickS3 KVM
 * Handles ANSI escape sequences, DEC graphics, cursor positioning,
 * alternate screen buffer (top, vi, nano), keyboard mapping,
 * and bi-directional WebSocket streaming.
 */
class Terminal {
    constructor(options = {}) {
        this.options = Object.assign({
            cols: 80,
            rows: 24,
            cursorBlink: true,
            theme: {
                background: '#0a0e14',
                foreground: '#c9d1d9',
                cursor: '#00d2ff'
            }
        }, options);

        this.cols = this.options.cols;
        this.rows = this.options.rows;

        this.screen = [];
        this.altScreen = null;
        this.normalScreen = null;
        this.isAltScreen = false;
        this.scrollback = [];
        this.maxScrollback = 1000;

        this.cursorX = 0;
        this.cursorY = 0;
        this.savedCursor = { x: 0, y: 0 };
        this.cursorVisible = true;
        this.appCursorKeys = false;

        // Active SGR attributes
        this.curFg = '';
        this.curBg = '';
        this.curBold = false;
        this.curDim = false;
        this.curUnderline = false;
        this.curReverse = false;

        // State machine: 0: Normal, 1: ESC, 2: CSI, 3: G0, 4: G1, 5: OSC
        this.state = 0;
        this.csiParam = '';
        this.charsetG0 = 'B'; // 'B': ASCII, '0': DEC Special Graphics

        this.onDataCallbacks = [];
        this.renderScheduled = false;

        this.element = null;
        this.outputElem = null;

        this.initGrid();
    }

    createEmptyCell() {
        return { ch: ' ', fg: '', bg: '', b: false, d: false, u: false, rev: false };
    }

    createEmptyRow() {
        const row = [];
        for (let c = 0; c < this.cols; c++) {
            row.push(this.createEmptyCell());
        }
        return row;
    }

    initGrid() {
        this.screen = [];
        for (let r = 0; r < this.rows; r++) {
            this.screen.push(this.createEmptyRow());
        }
    }

    measureCharSize() {
        const span = document.createElement('span');
        span.style.fontFamily = '"Fira Code", Consolas, Monaco, "Courier New", monospace';
        span.style.fontSize = '13px';
        span.style.lineHeight = '1.4';
        span.style.visibility = 'hidden';
        span.style.position = 'absolute';
        span.style.whiteSpace = 'pre';
        span.textContent = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
        document.body.appendChild(span);
        const rect = span.getBoundingClientRect();
        const w = rect.width / 62;
        const h = rect.height || 18.2;
        document.body.removeChild(span);
        return {
            charWidth: (w > 4 && w < 20) ? w : 7.8,
            charHeight: (h > 10 && h < 40) ? h : 18.2
        };
    }

    fit() {
        if (!this.element) return;
        const { charWidth, charHeight } = this.measureCharSize();
        const pad = 24;
        const availW = Math.max(200, this.element.clientWidth - pad);
        const availH = Math.max(150, this.element.clientHeight - pad);
        const newCols = Math.max(80, Math.floor(availW / charWidth));
        const newRows = Math.max(24, Math.floor(availH / charHeight));
        this.resize(newCols, newRows);
    }

    resize(newCols, newRows) {
        if (newCols === this.cols && newRows === this.rows) return;

        // Adjust screen rows
        while (this.screen.length < newRows) {
            this.screen.push(this.createEmptyRow());
        }
        while (this.screen.length > newRows) {
            if (!this.isAltScreen && this.screen.length > 0) {
                this.scrollback.push(this.screen.shift());
            } else {
                this.screen.pop();
            }
        }

        // Adjust column widths
        for (let r = 0; r < this.screen.length; r++) {
            while (this.screen[r].length < newCols) {
                this.screen[r].push(this.createEmptyCell());
            }
            if (this.screen[r].length > newCols) {
                this.screen[r].length = newCols;
            }
        }

        this.cols = newCols;
        this.rows = newRows;
        this.cursorX = Math.min(this.cursorX, this.cols - 1);
        this.cursorY = Math.min(this.cursorY, this.rows - 1);
        this.scheduleRender();
    }

    open(container) {
        if (typeof container === 'string') {
            container = document.getElementById(container);
        }
        if (!container) return;
        this.element = container;
        this.element.classList.add('xterm-container');
        this.element.style.background = this.options.theme.background;
        this.element.style.color = this.options.theme.foreground;
        this.element.tabIndex = 0;

        this.outputElem = document.createElement('div');
        this.outputElem.className = 'xterm-output';
        this.element.innerHTML = '';
        this.element.appendChild(this.outputElem);

        this.element.addEventListener('keydown', (e) => this.handleKeyDown(e));
        this.element.addEventListener('paste', (e) => this.handlePaste(e));
        this.element.focus();

        this.fit();
        window.addEventListener('resize', () => this.fit());

        this.render();
    }


    onData(callback) {
        this.onDataCallbacks.push(callback);
    }

    send(data) {
        this.onDataCallbacks.forEach(cb => cb(data));
    }

    lineFeed() {
        this.cursorY++;
        if (this.cursorY >= this.rows) {
            this.cursorY = this.rows - 1;
            if (!this.isAltScreen) {
                // Push top row to scrollback
                this.scrollback.push(this.screen[0]);
                if (this.scrollback.length > this.maxScrollback) {
                    this.scrollback.shift();
                }
            }
            this.screen.shift();
            this.screen.push(this.createEmptyRow());
        }
    }

    translateDecChar(ch) {
        if (this.charsetG0 === '0') {
            const boxMap = {
                '`': '◆', 'a': '▒', 'b': '␉', 'c': '␌', 'd': '␍', 'e': '␊', 'f': '°', 'g': '±',
                'h': '␤', 'i': '␋', 'j': '┘', 'k': '┐', 'l': '┌', 'm': '└', 'n': '┼', 'o': '⎺',
                'p': '⎻', 'q': '─', 'r': '─', 's': '⎽', 't': '├', 'u': '┤', 'v': '┴', 'w': '┬',
                'x': '│', 'y': '≤', 'z': '≥', '{': 'π', '|': '≠', '}': '£', '~': '·'
            };
            return boxMap[ch] || ch;
        }
        return ch;
    }

    write(data) {
        if (!data) return;
        const str = (typeof data === 'string') ? data : String(data);

        for (let i = 0; i < str.length; i++) {
            const ch = str[i];

            switch (this.state) {
                case 0: // Normal Text
                    if (ch === '\x1b') {
                        this.state = 1;
                    } else if (ch === '\r') {
                        this.cursorX = 0;
                    } else if (ch === '\n') {
                        this.lineFeed();
                    } else if (ch === '\b' || ch === '\x7f') {
                        if (this.cursorX > 0) this.cursorX--;
                    } else if (ch === '\t') {
                        this.cursorX = Math.min(this.cols - 1, (Math.floor(this.cursorX / 8) + 1) * 8);
                    } else if (ch === '\x07') {
                        // Bell - ignore
                    } else if (ch >= ' ') {
                        if (this.cursorX >= this.cols) {
                            this.cursorX = 0;
                            this.lineFeed();
                        }
                        const cellChar = this.translateDecChar(ch);
                        const row = this.screen[this.cursorY];
                        if (row && this.cursorX < this.cols) {
                            row[this.cursorX] = {
                                ch: cellChar,
                                fg: this.curFg,
                                bg: this.curBg,
                                b: this.curBold,
                                d: this.curDim,
                                u: this.curUnderline,
                                rev: this.curReverse
                            };
                        }
                        this.cursorX++;
                    }
                    break;

                case 1: // ESC Received
                    if (ch === '[') {
                        this.state = 2;
                        this.csiParam = '';
                    } else if (ch === '(') {
                        this.state = 3; // G0 charset
                    } else if (ch === ')') {
                        this.state = 4; // G1 charset
                    } else if (ch === ']') {
                        this.state = 5; // OSC
                    } else if (ch === '7') {
                        this.savedCursor = { x: this.cursorX, y: this.cursorY };
                        this.state = 0;
                    } else if (ch === '8') {
                        this.cursorX = this.savedCursor.x;
                        this.cursorY = this.savedCursor.y;
                        this.state = 0;
                    } else if (ch === 'M') {
                        if (this.cursorY > 0) this.cursorY--;
                        this.state = 0;
                    } else if (ch === 'c') {
                        this.initGrid();
                        this.cursorX = 0;
                        this.cursorY = 0;
                        this.state = 0;
                    } else {
                        this.state = 0; // Consume unknown 2-byte sequence
                    }
                    break;

                case 2: // CSI Parameter Collection: \x1b[
                    if ((ch >= '0' && ch <= '9') || ch === ';' || ch === '?' || ch === '>' || ch === ' ' || ch === '$') {
                        this.csiParam += ch;
                    } else {
                        // Terminator reached
                        this.execCsi(ch, this.csiParam);
                        this.state = 0;
                    }
                    break;

                case 3: // Charset G0 Selection: \x1b(
                    this.charsetG0 = ch; // 'B' = ASCII, '0' = DEC Box Graphics
                    this.state = 0;
                    break;

                case 4: // Charset G1 Selection: \x1b)
                    this.charsetG1 = ch;
                    this.state = 0;
                    break;

                case 5: // OSC String: \x1b]
                    if (ch === '\x07' || (ch === '\\' && str[i - 1] === '\x1b')) {
                        this.state = 0;
                    }
                    break;
            }
        }

        this.scheduleRender();
    }

    writeln(data) {
        this.write(data + '\r\n');
    }

    clear() {
        this.initGrid();
        this.scrollback = [];
        this.cursorX = 0;
        this.cursorY = 0;
        if (this.element) this.element.scrollTop = 0;
        this.scheduleRender();
    }

    execCsi(cmd, param) {
        const isPrivate = param.startsWith('?');
        const cleanParam = isPrivate ? param.substring(1) : param;
        const args = cleanParam ? cleanParam.split(';').map(v => parseInt(v, 10)) : [];

        switch (cmd) {
            case 'H':
            case 'f': { // Cursor Position [row;colH]
                const r = (args[0] || 1) - 1;
                const c = (args[1] || 1) - 1;
                this.cursorY = Math.max(0, Math.min(this.rows - 1, r));
                this.cursorX = Math.max(0, Math.min(this.cols - 1, c));
                break;
            }
            case 'A': { // Cursor Up
                const n = args[0] || 1;
                this.cursorY = Math.max(0, this.cursorY - n);
                break;
            }
            case 'B': { // Cursor Down
                const n = args[0] || 1;
                this.cursorY = Math.min(this.rows - 1, this.cursorY + n);
                break;
            }
            case 'C': { // Cursor Forward / Right
                const n = args[0] || 1;
                this.cursorX = Math.min(this.cols - 1, this.cursorX + n);
                break;
            }
            case 'D': { // Cursor Backward / Left
                const n = args[0] || 1;
                this.cursorX = Math.max(0, this.cursorX - n);
                break;
            }
            case 'G':
            case '`': { // Cursor Horizontal Absolute
                const c = (args[0] || 1) - 1;
                this.cursorX = Math.max(0, Math.min(this.cols - 1, c));
                break;
            }
            case 'd': { // Cursor Vertical Absolute
                const r = (args[0] || 1) - 1;
                this.cursorY = Math.max(0, Math.min(this.rows - 1, r));
                break;
            }
            case 'J': { // Erase in Display
                const mode = args[0] || 0;
                if (mode === 2 || mode === 3) {
                    for (let r = 0; r < this.rows; r++) {
                        this.screen[r] = this.createEmptyRow();
                    }
                    if (mode === 3 || (this.cursorX === 0 && this.cursorY === 0)) {
                        this.scrollback = [];
                        if (this.element) this.element.scrollTop = 0;
                    }
                } else if (mode === 0) {
                    if (this.cursorX === 0 && this.cursorY === 0) {
                        for (let r = 0; r < this.rows; r++) {
                            this.screen[r] = this.createEmptyRow();
                        }
                        this.scrollback = [];
                        if (this.element) this.element.scrollTop = 0;
                    } else {
                        for (let c = this.cursorX; c < this.cols; c++) this.screen[this.cursorY][c] = this.createEmptyCell();
                        for (let r = this.cursorY + 1; r < this.rows; r++) this.screen[r] = this.createEmptyRow();
                    }
                } else if (mode === 1) {
                    for (let r = 0; r < this.cursorY; r++) this.screen[r] = this.createEmptyRow();
                    for (let c = 0; c <= this.cursorX; c++) this.screen[this.cursorY][c] = this.createEmptyCell();
                }
                break;
            }

            case 'K': { // Erase in Line
                const mode = args[0] || 0;
                if (mode === 0) {
                    for (let c = this.cursorX; c < this.cols; c++) this.screen[this.cursorY][c] = this.createEmptyCell();
                } else if (mode === 1) {
                    for (let c = 0; c <= this.cursorX; c++) this.screen[this.cursorY][c] = this.createEmptyCell();
                } else if (mode === 2) {
                    this.screen[this.cursorY] = this.createEmptyRow();
                }
                break;
            }
            case 'm': // SGR Formatting
                this.handleSgr(args);
                break;

            case 'h': // Set Mode
                if (isPrivate) {
                    if (args.includes(1049) || args.includes(47)) {
                        // Enter Alternate Screen Buffer (used by top, vi, nano)
                        if (!this.isAltScreen) {
                            this.altScreen = [];
                            for (let r = 0; r < this.rows; r++) this.altScreen.push(this.createEmptyRow());
                            this.normalScreen = this.screen;
                            this.screen = this.altScreen;
                            this.isAltScreen = true;
                            this.cursorX = 0;
                            this.cursorY = 0;
                        }
                    } else if (args.includes(25)) {
                        this.cursorVisible = true;
                    } else if (args.includes(1)) {
                        this.appCursorKeys = true;
                    }
                }
                break;

            case 'l': // Reset Mode
                if (isPrivate) {
                    if (args.includes(1049) || args.includes(47)) {
                        // Leave Alternate Screen Buffer
                        if (this.isAltScreen && this.normalScreen) {
                            this.screen = this.normalScreen;
                            this.altScreen = null;
                            this.isAltScreen = false;
                        }
                    } else if (args.includes(25)) {
                        this.cursorVisible = false;
                    } else if (args.includes(1)) {
                        this.appCursorKeys = false;
                    }
                }
                break;

            case 's': // Save cursor position
                this.savedCursor = { x: this.cursorX, y: this.cursorY };
                break;

            case 'u': // Restore cursor position
                this.cursorX = this.savedCursor.x;
                this.cursorY = this.savedCursor.y;
                break;

            case 'S': { // Scroll Up
                const n = args[0] || 1;
                for (let k = 0; k < n; k++) this.lineFeed();
                break;
            }
        }
    }

    handleSgr(args) {
        if (args.length === 0) args = [0];

        const colors = [
            '#000000', '#ff5555', '#50fa7b', '#f1fa8c',
            '#bd93f9', '#ff79c6', '#8be9fd', '#c9d1d9'
        ];
        const brightColors = [
            '#6e7681', '#ff7b72', '#3fb950', '#d29922',
            '#a371f7', '#d2a8ff', '#56d4dd', '#f0f6fc'
        ];

        for (let i = 0; i < args.length; i++) {
            const code = isNaN(args[i]) ? 0 : args[i];

            if (code === 0) {
                this.curFg = '';
                this.curBg = '';
                this.curBold = false;
                this.curDim = false;
                this.curUnderline = false;
                this.curReverse = false;
            } else if (code === 1) {
                this.curBold = true;
            } else if (code === 2) {
                this.curDim = true;
            } else if (code === 4) {
                this.curUnderline = true;
            } else if (code === 7) {
                this.curReverse = true;
            } else if (code === 22) {
                this.curBold = false;
                this.curDim = false;
            } else if (code === 24) {
                this.curUnderline = false;
            } else if (code === 27) {
                this.curReverse = false;
            } else if (code >= 30 && code <= 37) {
                this.curFg = colors[code - 30];
            } else if (code === 39) {
                this.curFg = '';
            } else if (code >= 40 && code <= 47) {
                this.curBg = colors[code - 40];
            } else if (code === 49) {
                this.curBg = '';
            } else if (code >= 90 && code <= 97) {
                this.curFg = brightColors[code - 90];
            } else if (code >= 100 && code <= 107) {
                this.curBg = brightColors[code - 100];
            }
        }
    }

    scheduleRender() {
        if (!this.renderScheduled) {
            this.renderScheduled = true;
            requestAnimationFrame(() => {
                this.render();
                this.renderScheduled = false;
            });
        }
    }

    renderRowHtml(row, isCursorRow) {
        let html = '';
        for (let c = 0; c < this.cols; c++) {
            const cell = row[c] || { ch: ' ' };
            const isCursor = isCursorRow && this.cursorVisible && (c === this.cursorX);

            let fg = cell.fg;
            let bg = cell.bg;
            if (cell.rev) {
                const tmp = fg;
                fg = bg || this.options.theme.foreground;
                bg = tmp || this.options.theme.background;
            }

            let style = '';
            if (fg) style += `color:${fg};`;
            if (bg) style += `background-color:${bg};`;
            if (cell.b) style += 'font-weight:bold;';
            if (cell.d) style += 'opacity:0.65;';
            if (cell.u) style += 'text-decoration:underline;';

            let ch = cell.ch || ' ';
            if (ch === '&') ch = '&amp;';
            else if (ch === '<') ch = '&lt;';
            else if (ch === '>') ch = '&gt;';

            if (isCursor) {
                html += `<span class="term-cursor" style="${style}">${ch}</span>`;
            } else if (style) {
                html += `<span style="${style}">${ch}</span>`;
            } else {
                html += ch;
            }
        }

        if (isCursorRow && this.cursorVisible && this.cursorX >= this.cols) {
            html += `<span class="term-cursor">&nbsp;</span>`;
        }

        return `<div class="term-row">${html}</div>`;
    }

    render() {
        if (!this.outputElem) return;

        let fullHtml = '';

        // If not in alternate screen buffer, render scrollback history
        if (!this.isAltScreen && this.scrollback.length > 0) {
            for (let i = 0; i < this.scrollback.length; i++) {
                fullHtml += this.renderRowHtml(this.scrollback[i], false);
            }
        }

        // Render active 24-row screen grid
        for (let r = 0; r < this.rows; r++) {
            const isCursorRow = (r === this.cursorY);
            fullHtml += this.renderRowHtml(this.screen[r], isCursorRow);
        }

        this.outputElem.innerHTML = fullHtml;

        if (this.element && !this.isAltScreen) {
            this.element.scrollTop = this.element.scrollHeight;
        }
    }

    handleKeyDown(e) {
        let toSend = null;

        if (e.ctrlKey) {
            const key = e.key.toUpperCase();
            if (key >= 'A' && key <= 'Z') {
                toSend = String.fromCharCode(key.charCodeAt(0) - 64);
            } else if (e.key === 'c' || e.key === 'C') {
                toSend = '\x03';
            } else if (e.key === 'd' || e.key === 'D') {
                toSend = '\x04';
            } else if (e.key === 'z' || e.key === 'Z') {
                toSend = '\x1a';
            }
        } else {
            switch (e.key) {
                case 'Enter': toSend = '\r'; break;
                case 'Backspace': toSend = '\x7f'; break;
                case 'Tab': toSend = '\t'; e.preventDefault(); break;
                case 'Escape': toSend = '\x1b'; break;
                case 'ArrowUp': toSend = this.appCursorKeys ? '\x1bOA' : '\x1b[A'; break;
                case 'ArrowDown': toSend = this.appCursorKeys ? '\x1bOB' : '\x1b[B'; break;
                case 'ArrowRight': toSend = this.appCursorKeys ? '\x1bOC' : '\x1b[C'; break;
                case 'ArrowLeft': toSend = this.appCursorKeys ? '\x1bOD' : '\x1b[D'; break;
                case 'Home': toSend = '\x1b[H'; break;
                case 'End': toSend = '\x1b[F'; break;
                case 'PageUp': toSend = '\x1b[5~'; break;
                case 'PageDown': toSend = '\x1b[6~'; break;
                default:
                    if (e.key.length === 1 && !e.altKey && !e.metaKey) {
                        toSend = e.key;
                    }
                    break;
            }
        }

        if (toSend !== null) {
            e.preventDefault();
            this.send(toSend);
        }
    }

    handlePaste(e) {
        e.preventDefault();
        const text = (e.clipboardData || window.clipboardData).getData('text');
        if (text) {
            this.send(text);
        }
    }
}
