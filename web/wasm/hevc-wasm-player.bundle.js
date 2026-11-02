var __defProp = Object.defineProperty;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __esm = (fn, res, err) => function __init() {
  if (err) throw err[0];
  try {
    return fn && (res = (0, fn[__getOwnPropNames(fn)[0]])(fn = 0)), res;
  } catch (e) {
    throw err = [e], e;
  }
};
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};

// scratch/wasm_test/package/dist/esm/annexb.js
function concat(parts) {
  const size = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
}
function toUint8Array(data) {
  return data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
}
function startCodeAt(data, index) {
  if (data[index] !== 0 || data[index + 1] !== 0)
    return 0;
  if (data[index + 2] === 1)
    return 3;
  return data[index + 2] === 0 && data[index + 3] === 1 ? 4 : 0;
}
function findStartCode(data, from) {
  for (let i = from; i + 2 < data.length; i += 1) {
    if (startCodeAt(data, i))
      return i;
  }
  return -1;
}
function nalType(nal) {
  const offset = startCodeAt(nal, 0);
  return offset && nal.length >= offset + 2 ? nal[offset] >> 1 & 63 : -1;
}
function firstSlice(nal) {
  const offset = startCodeAt(nal, 0);
  return offset > 0 && nal.length > offset + 2 && (nal[offset + 2] & 128) !== 0;
}
function isPrefix(type) {
  return type === VPS || type === SPS || type === PPS || type === AUD || type === PREFIX_SEI;
}
function hasType(nals, expected) {
  return nals.some((nal) => nalType(nal) === expected);
}
function isHevcKeyAccessUnit(data) {
  const bytes = toUint8Array(data);
  let offset = findStartCode(bytes, 0);
  while (offset >= 0) {
    const type = nalType(bytes.subarray(offset));
    if (type >= IRAP_MIN && type <= IRAP_MAX)
      return true;
    const codeLength = startCodeAt(bytes, offset);
    offset = findStartCode(bytes, offset + codeLength);
  }
  return false;
}
var VCL_MAX, IRAP_MIN, IRAP_MAX, VPS, SPS, PPS, AUD, EOS, EOB, PREFIX_SEI, AnnexBAccessUnitParser, hevcNalType;
var init_annexb = __esm({
  "scratch/wasm_test/package/dist/esm/annexb.js"() {
    VCL_MAX = 31;
    IRAP_MIN = 16;
    IRAP_MAX = 23;
    VPS = 32;
    SPS = 33;
    PPS = 34;
    AUD = 35;
    EOS = 36;
    EOB = 37;
    PREFIX_SEI = 39;
    AnnexBAccessUnitParser = class {
      #storage = new Uint8Array(4096);
      #length = 0;
      #current = [];
      #nextPrefix = [];
      #hasVcl = false;
      #parameterSets = /* @__PURE__ */ new Map();
      push(data) {
        const input = toUint8Array(data);
        this.#append(input);
        return this.#drainNals(false);
      }
      flush() {
        const output = this.#drainNals(true);
        const final = this.#emitCurrent();
        if (final)
          output.push(final);
        this.#length = 0;
        this.#nextPrefix = [];
        return output;
      }
      reset() {
        this.#length = 0;
        if (this.#storage.byteLength > 1024 * 1024) {
          this.#storage = new Uint8Array(4096);
        }
        this.#current = [];
        this.#nextPrefix = [];
        this.#hasVcl = false;
        this.#parameterSets.clear();
      }
      #drainNals(flush) {
        const output = [];
        let bytes = this.#storage.subarray(0, this.#length);
        let first = findStartCode(bytes, 0);
        if (first < 0) {
          if (this.#length > 3)
            this.#keepRemainder(this.#length - 3);
          return output;
        }
        if (first > 0) {
          this.#keepRemainder(first);
          bytes = this.#storage.subarray(0, this.#length);
        }
        let cursor = 0;
        while (true) {
          const codeLength = startCodeAt(bytes, cursor);
          const next = findStartCode(bytes, cursor + codeLength);
          if (next < 0) {
            if (flush && bytes.length > cursor + codeLength) {
              const emitted2 = this.#acceptNal(bytes.slice(cursor));
              if (emitted2)
                output.push(emitted2);
              this.#length = 0;
            } else {
              this.#keepRemainder(cursor);
            }
            break;
          }
          const emitted = this.#acceptNal(bytes.slice(cursor, next));
          if (emitted)
            output.push(emitted);
          cursor = next;
        }
        return output;
      }
      #append(input) {
        const required = this.#length + input.byteLength;
        if (required > this.#storage.byteLength) {
          let capacity = this.#storage.byteLength;
          while (capacity < required)
            capacity *= 2;
          const expanded = new Uint8Array(capacity);
          expanded.set(this.#storage.subarray(0, this.#length));
          this.#storage = expanded;
        }
        this.#storage.set(input, this.#length);
        this.#length = required;
      }
      #keepRemainder(from) {
        if (from <= 0)
          return;
        this.#storage.copyWithin(0, from, this.#length);
        this.#length -= from;
      }
      #acceptNal(nal) {
        const type = nalType(nal);
        if (type < 0)
          return null;
        if (type === VPS || type === SPS || type === PPS) {
          this.#parameterSets.set(type, nal.slice());
        }
        if (type === AUD && (this.#hasVcl || this.#current.length)) {
          const accessUnit = this.#emitCurrent();
          this.#current.push(...this.#nextPrefix, nal);
          this.#nextPrefix = [];
          return accessUnit;
        }
        if (type <= VCL_MAX && firstSlice(nal) && this.#hasVcl) {
          const accessUnit = this.#emitCurrent();
          this.#current.push(...this.#nextPrefix, nal);
          this.#nextPrefix = [];
          this.#hasVcl = true;
          return accessUnit;
        }
        if (this.#hasVcl && isPrefix(type)) {
          this.#nextPrefix.push(nal);
        } else {
          this.#current.push(nal);
          if (type <= VCL_MAX)
            this.#hasVcl = true;
        }
        if ((type === EOS || type === EOB) && this.#hasVcl) {
          return this.#emitCurrent();
        }
        return null;
      }
      #emitCurrent() {
        if (!this.#current.length || !this.#hasVcl)
          return null;
        const key = this.#current.some((nal) => {
          const type = nalType(nal);
          return type >= IRAP_MIN && type <= IRAP_MAX;
        });
        let nals = this.#current;
        if (key) {
          const missing = [VPS, SPS, PPS].filter((type) => !hasType(nals, type)).map((type) => this.#parameterSets.get(type)).filter((nal) => nal !== void 0);
          if (missing.length)
            nals = [...missing, ...nals];
        }
        const accessUnit = { data: concat(nals), key };
        this.#current = [];
        this.#hasVcl = false;
        return accessUnit;
      }
    };
    hevcNalType = nalType;
  }
});

// scratch/wasm_test/package/dist/esm/errors.js
var HevcPlayerError;
var init_errors = __esm({
  "scratch/wasm_test/package/dist/esm/errors.js"() {
    HevcPlayerError = class extends Error {
      code;
      constructor(code, message, cause) {
        super(message, cause === void 0 ? void 0 : { cause });
        this.name = "HevcPlayerError";
        this.code = code;
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/renderer.js
function yuvConversion(matrixCoefficients, height) {
  if (matrixCoefficients === 1 || matrixCoefficients === 2 && height > 576) {
    return { rV: 1.5748, gU: -0.187324, gV: -0.468124, bU: 1.8556 };
  }
  if (matrixCoefficients === 9 || matrixCoefficients === 10) {
    return { rV: 1.4746, gU: -0.164553, gV: -0.571353, bU: 1.8814 };
  }
  return { rV: 1.402, gU: -0.344136, gV: -0.714136, bU: 1.772 };
}
function normalizeSample(sample, bitDepth, chroma, fullRange) {
  const maximum = (1 << bitDepth) - 1;
  const normalized = sample / maximum;
  if (fullRange) {
    const chromaOffset = chroma ? (1 << bitDepth - 1) / maximum : 0;
    return normalized - chromaOffset;
  }
  const shift = 1 << Math.max(0, bitDepth - 8);
  const offset = (chroma ? 128 : 16) * shift / maximum;
  const span = (chroma ? 224 : 219) * shift / maximum;
  return (normalized - offset) / span;
}
function createRenderer(canvas, mode = "auto") {
  if (mode !== "canvas2d") {
    try {
      return new WebGLRenderer(canvas);
    } catch (error) {
      if (mode === "webgl")
        throw error;
      if (!(error instanceof HevcPlayerError) || error.code !== "WEBGL_UNSUPPORTED") {
        throw error;
      }
    }
  }
  return new CanvasRenderer(canvas);
}
var CanvasRenderer, VERTEX_SHADER, FRAGMENT_SHADER_8, FRAGMENT_SHADER_16, WebGLRenderer;
var init_renderer = __esm({
  "scratch/wasm_test/package/dist/esm/renderer.js"() {
    init_errors();
    CanvasRenderer = class {
      kind = "canvas2d";
      canvas;
      context;
      imageData = null;
      constructor(canvas) {
        if (!canvas?.getContext) {
          throw new HevcPlayerError("INVALID_CANVAS", "A canvas element is required.");
        }
        this.canvas = canvas;
        const context = canvas.getContext("2d", { alpha: false, desynchronized: true });
        if (!context) {
          throw new HevcPlayerError("CANVAS_CONTEXT", "Could not create a 2D canvas context.");
        }
        this.context = context;
      }
      render(frame) {
        if (frame.closed)
          return;
        const width = frame.displayWidth;
        const height = frame.displayHeight;
        if (this.canvas.width !== width || this.canvas.height !== height) {
          this.canvas.width = width;
          this.canvas.height = height;
          this.imageData = null;
        }
        if (frame._nativeFrame) {
          this.context.drawImage(frame._nativeFrame, 0, 0, width, height);
          return;
        }
        this.#renderI420(frame, width, height);
      }
      close() {
        this.imageData = null;
      }
      #renderI420(frame, width, height) {
        const planes = frame._planes;
        if (!planes) {
          throw new HevcPlayerError("FRAME_FORMAT", "WASM frame does not contain I420 planes.");
        }
        const { y, u, v, yStride, uStride, vStride, yWidth, uWidth, vWidth, yHeight, uHeight, vHeight, yBitDepth = 8, uBitDepth = 8, vBitDepth = 8 } = planes;
        if (yWidth <= 0 || yHeight <= 0 || uWidth <= 0 || uHeight <= 0 || vWidth <= 0 || vHeight <= 0) {
          throw new HevcPlayerError("FRAME_FORMAT", "WASM frame has invalid plane dimensions.");
        }
        if (!this.imageData || this.imageData.width !== width || this.imageData.height !== height) {
          this.imageData = this.context.createImageData(width, height);
        }
        const rgba = this.imageData.data;
        const commonI420 = yWidth === width && yHeight === height && uWidth === Math.ceil(width / 2) && vWidth === Math.ceil(width / 2) && uHeight === Math.ceil(height / 2) && vHeight === Math.ceil(height / 2);
        const yIs16 = yBitDepth > 8;
        const uIs16 = uBitDepth > 8;
        const vIs16 = vBitDepth > 8;
        const y16 = yIs16 ? new Uint16Array(y.buffer, y.byteOffset, y.byteLength / 2) : null;
        const u16 = uIs16 ? new Uint16Array(u.buffer, u.byteOffset, u.byteLength / 2) : null;
        const v16 = vIs16 ? new Uint16Array(v.buffer, v.byteOffset, v.byteLength / 2) : null;
        const fullRange = planes.fullRange ?? false;
        const conversion = yuvConversion(planes.matrixCoefficients ?? 2, height);
        let index = 0;
        for (let row = 0; row < height; row += 1) {
          const yRow = Math.min(yHeight - 1, commonI420 ? row : Math.floor(row * yHeight / height));
          const uRow = Math.min(uHeight - 1, commonI420 ? row >> 1 : Math.floor(row * uHeight / height));
          const vRow = Math.min(vHeight - 1, commonI420 ? row >> 1 : Math.floor(row * vHeight / height));
          const yBase = yIs16 ? yRow * (yStride >> 1) : yRow * yStride;
          const uBase = uIs16 ? uRow * (uStride >> 1) : uRow * uStride;
          const vBase = vIs16 ? vRow * (vStride >> 1) : vRow * vStride;
          for (let col = 0; col < width; col += 1) {
            const yCol = commonI420 ? col : Math.min(yWidth - 1, Math.floor(col * yWidth / width));
            const uCol = commonI420 ? col >> 1 : Math.min(uWidth - 1, Math.floor(col * uWidth / width));
            const vCol = commonI420 ? col >> 1 : Math.min(vWidth - 1, Math.floor(col * vWidth / width));
            const yy = normalizeSample(yIs16 ? y16[yBase + yCol] ?? 0 : y[yBase + yCol] ?? 0, yBitDepth, false, fullRange);
            const uu = normalizeSample(uIs16 ? u16[uBase + uCol] ?? 0 : u[uBase + uCol] ?? 0, uBitDepth, true, fullRange);
            const vv = normalizeSample(vIs16 ? v16[vBase + vCol] ?? 0 : v[vBase + vCol] ?? 0, vBitDepth, true, fullRange);
            rgba[index] = 255 * (yy + conversion.rV * vv);
            rgba[index + 1] = 255 * (yy + conversion.gU * uu + conversion.gV * vv);
            rgba[index + 2] = 255 * (yy + conversion.bU * uu);
            rgba[index + 3] = 255;
            index += 4;
          }
        }
        this.context.putImageData(this.imageData, 0, 0);
      }
    };
    VERTEX_SHADER = `#version 300 es
in vec2 aPosition;
in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
  gl_Position = vec4(aPosition, 0.0, 1.0);
  vTexCoord = aTexCoord;
}`;
    FRAGMENT_SHADER_8 = `#version 300 es
precision highp float;
in vec2 vTexCoord;
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
uniform vec2 uYScale;
uniform vec2 uUScale;
uniform vec2 uVScale;
uniform vec3 uRangeOffset;
uniform vec3 uRangeScale;
uniform mat3 uColorMatrix;
out vec4 outColor;
void main() {
  vec3 yuv = vec3(
    texture(uY, vTexCoord * uYScale).r,
    texture(uU, vTexCoord * uUScale).r,
    texture(uV, vTexCoord * uVScale).r
  );
  outColor = vec4(uColorMatrix * ((yuv - uRangeOffset) * uRangeScale), 1.0);
}`;
    FRAGMENT_SHADER_16 = `#version 300 es
precision highp float;
precision highp usampler2D;
in vec2 vTexCoord;
uniform usampler2D uY;
uniform usampler2D uU;
uniform usampler2D uV;
uniform vec2 uYScale;
uniform vec2 uUScale;
uniform vec2 uVScale;
uniform vec3 uSampleMax;
uniform vec3 uRangeOffset;
uniform vec3 uRangeScale;
uniform mat3 uColorMatrix;
out vec4 outColor;
void main() {
  vec3 yuv = vec3(
    float(texture(uY, vTexCoord * uYScale).r),
    float(texture(uU, vTexCoord * uUScale).r),
    float(texture(uV, vTexCoord * uVScale).r)
  ) / uSampleMax;
  outColor = vec4(uColorMatrix * ((yuv - uRangeOffset) * uRangeScale), 1.0);
}`;
    WebGLRenderer = class {
      kind = "webgl2";
      canvas;
      gl;
      programs;
      textures;
      vertexBuffer;
      closed = false;
      constructor(canvas) {
        this.canvas = canvas;
        const gl = canvas.getContext("webgl2", {
          alpha: false,
          antialias: false,
          depth: false,
          desynchronized: false,
          preserveDrawingBuffer: true
        });
        if (!gl) {
          throw new HevcPlayerError("WEBGL_UNSUPPORTED", "WebGL2 is not available.");
        }
        this.gl = gl;
        this.programs = [
          this.#createProgram(FRAGMENT_SHADER_8),
          this.#createProgram(FRAGMENT_SHADER_16)
        ];
        const vertexBuffer = gl.createBuffer();
        if (!vertexBuffer) {
          throw new HevcPlayerError("WEBGL_INIT", "Could not create the WebGL vertex buffer.");
        }
        this.vertexBuffer = vertexBuffer;
        gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
          -1,
          -1,
          0,
          1,
          1,
          -1,
          1,
          1,
          -1,
          1,
          0,
          0,
          1,
          1,
          1,
          0
        ]), gl.STATIC_DRAW);
        const textures = [gl.createTexture(), gl.createTexture(), gl.createTexture()];
        if (textures.some((texture) => texture === null)) {
          throw new HevcPlayerError("WEBGL_INIT", "Could not create I420 textures.");
        }
        this.textures = textures;
        for (const texture of this.textures) {
          gl.bindTexture(gl.TEXTURE_2D, texture);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        }
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      }
      render(frame) {
        if (this.closed || frame.closed)
          return;
        const planes = frame._planes;
        if (!planes) {
          throw new HevcPlayerError("FRAME_FORMAT", "WebGL rendering requires WASM I420 planes.");
        }
        const gl = this.gl;
        const width = frame.displayWidth;
        const height = frame.displayHeight;
        if (this.canvas.width !== width || this.canvas.height !== height) {
          this.canvas.width = width;
          this.canvas.height = height;
        }
        gl.viewport(0, 0, width, height);
        const is16 = planes.yBitDepth > 8 || planes.uBitDepth > 8 || planes.vBitDepth > 8;
        if (is16 && (planes.yBitDepth <= 8 || planes.uBitDepth <= 8 || planes.vBitDepth <= 8)) {
          throw new HevcPlayerError("FRAME_FORMAT", "WebGL rendering requires matching sample widths across I420 planes.");
        }
        const state = this.programs[is16 ? 1 : 0];
        gl.useProgram(state.program);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
        gl.enableVertexAttribArray(state.position);
        gl.vertexAttribPointer(state.position, 2, gl.FLOAT, false, 16, 0);
        gl.enableVertexAttribArray(state.texCoord);
        gl.vertexAttribPointer(state.texCoord, 2, gl.FLOAT, false, 16, 8);
        const data = [planes.y, planes.u, planes.v];
        const strides = [planes.yStride, planes.uStride, planes.vStride];
        const widths = [planes.yWidth, planes.uWidth, planes.vWidth];
        const heights = [planes.yHeight, planes.uHeight, planes.vHeight];
        const bitDepths = [planes.yBitDepth, planes.uBitDepth, planes.vBitDepth];
        for (let index = 0; index < 3; index += 1) {
          const bytesPerSample = bitDepths[index] > 8 ? 2 : 1;
          const textureWidth = strides[index] / bytesPerSample;
          const samples = bytesPerSample === 2 ? new Uint16Array(data[index].buffer, data[index].byteOffset, data[index].byteLength / 2) : data[index];
          gl.activeTexture(gl.TEXTURE0 + index);
          gl.bindTexture(gl.TEXTURE_2D, this.textures[index]);
          if (is16) {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R16UI, textureWidth, heights[index], 0, gl.RED_INTEGER, gl.UNSIGNED_SHORT, samples);
          } else {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, textureWidth, heights[index], 0, gl.RED, gl.UNSIGNED_BYTE, samples);
          }
          gl.uniform2f(state.scales[index], widths[index] / textureWidth, 1);
        }
        if (state.sampleMax) {
          gl.uniform3f(state.sampleMax, (1 << planes.yBitDepth) - 1, (1 << planes.uBitDepth) - 1, (1 << planes.vBitDepth) - 1);
        }
        const maxima = bitDepths.map((depth) => (1 << depth) - 1);
        const offsets = bitDepths.map((depth, index) => {
          if (planes.fullRange) {
            return index === 0 ? 0 : (1 << depth - 1) / ((1 << depth) - 1);
          }
          return (index === 0 ? 16 : 128) * (1 << Math.max(0, depth - 8)) / maxima[index];
        });
        const rangeScales = bitDepths.map((depth, index) => {
          if (planes.fullRange)
            return 1;
          return maxima[index] / ((index === 0 ? 219 : 224) * (1 << Math.max(0, depth - 8)));
        });
        gl.uniform3f(state.rangeOffset, offsets[0], offsets[1], offsets[2]);
        gl.uniform3f(state.rangeScale, rangeScales[0], rangeScales[1], rangeScales[2]);
        const conversion = yuvConversion(planes.matrixCoefficients ?? 2, height);
        gl.uniformMatrix3fv(state.colorMatrix, false, new Float32Array([
          1,
          1,
          1,
          0,
          conversion.gU,
          conversion.bU,
          conversion.rV,
          conversion.gV,
          0
        ]));
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      }
      close() {
        if (this.closed)
          return;
        this.closed = true;
        for (const texture of this.textures)
          this.gl.deleteTexture(texture);
        for (const state of this.programs)
          this.gl.deleteProgram(state.program);
        this.gl.deleteBuffer(this.vertexBuffer);
      }
      #createProgram(fragmentSource) {
        const gl = this.gl;
        const vertex = this.#compileShader(gl.VERTEX_SHADER, VERTEX_SHADER);
        const fragment = this.#compileShader(gl.FRAGMENT_SHADER, fragmentSource);
        const program = gl.createProgram();
        if (!program) {
          throw new HevcPlayerError("WEBGL_INIT", "Could not create a WebGL program.");
        }
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        gl.linkProgram(program);
        gl.deleteShader(vertex);
        gl.deleteShader(fragment);
        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
          const detail = gl.getProgramInfoLog(program) ?? "unknown link error";
          gl.deleteProgram(program);
          throw new HevcPlayerError("WEBGL_SHADER", `Could not link I420 shader: ${detail}`);
        }
        const position = gl.getAttribLocation(program, "aPosition");
        const texCoord = gl.getAttribLocation(program, "aTexCoord");
        const scales = ["uYScale", "uUScale", "uVScale"].map((name) => {
          const location = gl.getUniformLocation(program, name);
          if (!location) {
            throw new HevcPlayerError("WEBGL_SHADER", `Missing shader uniform ${name}.`);
          }
          return location;
        });
        gl.useProgram(program);
        for (const [index, name] of ["uY", "uU", "uV"].entries()) {
          gl.uniform1i(gl.getUniformLocation(program, name), index);
        }
        return {
          program,
          position,
          texCoord,
          scales,
          sampleMax: gl.getUniformLocation(program, "uSampleMax"),
          rangeOffset: this.#requiredUniform(program, "uRangeOffset"),
          rangeScale: this.#requiredUniform(program, "uRangeScale"),
          colorMatrix: this.#requiredUniform(program, "uColorMatrix")
        };
      }
      #requiredUniform(program, name) {
        const location = this.gl.getUniformLocation(program, name);
        if (!location)
          throw new HevcPlayerError("WEBGL_SHADER", `Missing shader uniform ${name}.`);
        return location;
      }
      #compileShader(type, source) {
        const shader = this.gl.createShader(type);
        if (!shader) {
          throw new HevcPlayerError("WEBGL_INIT", "Could not create a WebGL shader.");
        }
        this.gl.shaderSource(shader, source);
        this.gl.compileShader(shader);
        if (!this.gl.getShaderParameter(shader, this.gl.COMPILE_STATUS)) {
          const detail = this.gl.getShaderInfoLog(shader) ?? "unknown compile error";
          this.gl.deleteShader(shader);
          throw new HevcPlayerError("WEBGL_SHADER", `Could not compile I420 shader: ${detail}`);
        }
        return shader;
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/events.js
var EventHub;
var init_events = __esm({
  "scratch/wasm_test/package/dist/esm/events.js"() {
    EventHub = class {
      #listeners = /* @__PURE__ */ new Map();
      on(type, listener) {
        const listeners = this.#listeners.get(type) ?? /* @__PURE__ */ new Set();
        listeners.add(listener);
        this.#listeners.set(type, listeners);
        return () => this.off(type, listener);
      }
      off(type, listener) {
        this.#listeners.get(type)?.delete(listener);
      }
      emit(type, detail) {
        for (const listener of this.#listeners.get(type) ?? []) {
          listener(detail);
        }
      }
      clear() {
        this.#listeners.clear();
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/frame.js
var UnifiedVideoFrame;
var init_frame = __esm({
  "scratch/wasm_test/package/dist/esm/frame.js"() {
    UnifiedVideoFrame = class {
      timestamp;
      duration;
      codedWidth;
      codedHeight;
      displayWidth;
      displayHeight;
      backend;
      _nativeFrame;
      _planes;
      _closed = false;
      constructor(init) {
        this.timestamp = init.timestamp;
        this.duration = init.duration;
        this.codedWidth = init.codedWidth;
        this.codedHeight = init.codedHeight;
        this.displayWidth = init.displayWidth ?? init.codedWidth;
        this.displayHeight = init.displayHeight ?? init.codedHeight;
        this.backend = init.backend;
        this._nativeFrame = init.nativeFrame;
        this._planes = init.planes;
      }
      get closed() {
        return this._closed;
      }
      /** Native WebCodecs frame, available only while a WebCodecs frame is open. */
      get nativeFrame() {
        return this._nativeFrame;
      }
      /** Decoded I420 planes, available only while a WASM frame is open. */
      get planes() {
        return this._planes;
      }
      close() {
        if (this._closed)
          return;
        this._closed = true;
        this._nativeFrame?.close();
        this._nativeFrame = void 0;
        this._planes = void 0;
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/backends/webcodecs.js
function needsWebCodecsGeometryCorrection(frame, expectedDisplayWidth, expectedDisplayHeight) {
  if (!Number.isInteger(expectedDisplayWidth) || !Number.isInteger(expectedDisplayHeight) || expectedDisplayWidth <= 0 || expectedDisplayHeight <= 0) {
    return false;
  }
  if (expectedDisplayWidth > frame.codedWidth || expectedDisplayHeight > frame.codedHeight) {
    return false;
  }
  const rect = frame.visibleRect;
  if (rect === null)
    return true;
  return rect.x !== 0 || rect.y !== 0 || rect.width !== expectedDisplayWidth || rect.height !== expectedDisplayHeight || frame.displayWidth !== expectedDisplayWidth || frame.displayHeight !== expectedDisplayHeight;
}
function correctWebCodecsGeometry(nativeFrame, expectedDisplayWidth, expectedDisplayHeight) {
  if (!needsWebCodecsGeometryCorrection(nativeFrame, expectedDisplayWidth, expectedDisplayHeight)) {
    return nativeFrame;
  }
  try {
    const init = {
      visibleRect: {
        x: 0,
        y: 0,
        width: expectedDisplayWidth,
        height: expectedDisplayHeight
      },
      displayWidth: expectedDisplayWidth,
      displayHeight: expectedDisplayHeight
    };
    if (nativeFrame.duration !== null) {
      init.duration = nativeFrame.duration;
    }
    const correctedFrame = new globalThis.VideoFrame(nativeFrame, init);
    nativeFrame.close();
    return correctedFrame;
  } catch {
    return nativeFrame;
  }
}
var WebCodecsBackend;
var init_webcodecs = __esm({
  "scratch/wasm_test/package/dist/esm/backends/webcodecs.js"() {
    init_frame();
    init_errors();
    WebCodecsBackend = class _WebCodecsBackend {
      options;
      decoder = null;
      config = null;
      closed = false;
      constructor(options) {
        this.options = options;
      }
      get decodeQueueSize() {
        return this.decoder?.decodeQueueSize ?? 0;
      }
      static async isSupported(config) {
        if (typeof globalThis.VideoDecoder !== "function")
          return false;
        try {
          const result = await globalThis.VideoDecoder.isConfigSupported(config);
          return result.supported === true;
        } catch {
          return false;
        }
      }
      async init() {
        const config = {
          codec: this.options.codec,
          optimizeForLatency: true,
          hardwareAcceleration: this.options.hardwareAcceleration ?? "no-preference"
        };
        if (this.options.codedWidth !== void 0) {
          config.codedWidth = this.options.codedWidth;
        }
        if (this.options.codedHeight !== void 0) {
          config.codedHeight = this.options.codedHeight;
        }
        if (!await _WebCodecsBackend.isSupported(config)) {
          throw new HevcPlayerError("WEBCODECS_UNSUPPORTED", `WebCodecs does not support HEVC configuration "${config.codec}".`);
        }
        this.decoder = new globalThis.VideoDecoder({
          output: (nativeFrame) => {
            const outputFrame = correctWebCodecsGeometry(nativeFrame, this.options.expectedDisplayWidth, this.options.expectedDisplayHeight);
            const frameInit = {
              timestamp: outputFrame.timestamp,
              codedWidth: outputFrame.codedWidth,
              codedHeight: outputFrame.codedHeight,
              displayWidth: outputFrame.displayWidth,
              displayHeight: outputFrame.displayHeight,
              backend: "webcodecs",
              nativeFrame: outputFrame
            };
            const frame = new UnifiedVideoFrame(outputFrame.duration === null ? frameInit : { ...frameInit, duration: outputFrame.duration });
            this.options.onFrame(frame);
          },
          error: (error) => this.options.onError(new HevcPlayerError("WEBCODECS_DECODE", error.message, error))
        });
        this.decoder.addEventListener("dequeue", this.options.onDequeue);
        this.config = config;
        this.decoder.configure(config);
      }
      decode(accessUnit, timestamp, duration) {
        if (!this.decoder || this.closed) {
          throw new HevcPlayerError("DECODER_CLOSED", "WebCodecs decoder is not open.");
        }
        const init = {
          type: accessUnit.key ? "key" : "delta",
          timestamp,
          data: accessUnit.data
        };
        if (duration !== void 0)
          init.duration = duration;
        this.decoder.decode(new globalThis.EncodedVideoChunk(init));
      }
      async flush() {
        await this.decoder?.flush();
      }
      reset() {
        if (this.decoder?.state === "configured" && this.config) {
          this.decoder.reset();
          this.decoder.configure(this.config);
        }
      }
      close() {
        this.closed = true;
        if (this.decoder && this.decoder.state !== "closed") {
          this.decoder.removeEventListener("dequeue", this.options.onDequeue);
          this.decoder.close();
        }
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/backends/wasm.js
function supportsSimd() {
  return typeof WebAssembly === "object" && WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]));
}
function supportsThreads() {
  try {
    return typeof MessageChannel !== "undefined" &&
           typeof SharedArrayBuffer !== "undefined" &&
           typeof WebAssembly === "object" &&
           typeof WebAssembly.Memory === "function" &&
           new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }) instanceof WebAssembly.Memory;
  } catch {
    return false;
  }
}
function selectVariant(requested) {
  if (requested && requested !== "auto")
    return requested;
  if (supportsThreads() && supportsSimd())
    return "threads-simd";
  return supportsSimd() ? "simd" : "baseline";
}
function getWasmVariantCandidates(requested, selected) {
  if (requested === "threads-simd" || selected === "threads-simd") {
    return supportsSimd() ? ["threads-simd", "simd", "baseline"] : ["threads-simd", "baseline"];
  }
  if (requested && requested !== "auto") {
    return [requested, "baseline"];
  }
  return selected === "simd" ? ["simd", "baseline"] : ["baseline"];
}
var WasmBackend;
var init_wasm = __esm({
  "scratch/wasm_test/package/dist/esm/backends/wasm.js"() {
    init_frame();
    init_errors();
    WasmBackend = class {
      options;
      execution = "main";
      hooks;
      module = null;
      context = 0;
      closed = false;
      variant;
      constructor(options, hooks = {}) {
        this.options = options;
        this.hooks = hooks;
        this.variant = selectVariant(options.wasmVariant);
      }
      get decodeQueueSize() {
        return 0;
      }
      async init() {
        const configuredBase = this.options.wasmBaseUrl ?? new URL("../../wasm/", import.meta.url);
        const baseUrl = new URL(configuredBase, globalThis.document?.baseURI ?? import.meta.url).href;
        const candidates = getWasmVariantCandidates(this.options.wasmVariant, this.variant);
        let lastError;
        let selectedVariant = this.variant;
        for (const candidate of candidates) {
          const moduleUrl = new URL(`libde265-${candidate}.mjs`, baseUrl);
          try {
            const imported = await import(moduleUrl.href);
            const createModule = imported.default;
            this.module = await createModule({
              locateFile: (file) => new URL(file, baseUrl).href
            });
            selectedVariant = candidate;
            break;
          } catch (error) {
            const detail = error instanceof Error && error.message ? ` ${error.message}` : "";
            lastError = new HevcPlayerError("WASM_LOAD", `Unable to initialize ${moduleUrl.href}.${detail}`, error);
          }
        }
        if (!this.module) {
          throw lastError ?? new HevcPlayerError("WASM_LOAD", "No WASM variant could be loaded.");
        }
        this.variant = selectedVariant;
        const threads = selectedVariant === "threads-simd" ? Math.min(4, Math.max(1, this.options.threads ?? Math.min(4, globalThis.navigator?.hardwareConcurrency ?? 2))) : 0;
        this.context = this.module._hevc_decoder_create(threads);
        if (!this.context)
          throw new HevcPlayerError("WASM_INIT", "libde265 decoder creation failed.");
      }
      decode(accessUnit, timestamp) {
        const module = this.module;
        if (!module || !this.context || this.closed) {
          throw new HevcPlayerError("DECODER_CLOSED", "WASM decoder is not open.");
        }
        const data = accessUnit.data;
        const pointer = module._malloc(data.byteLength);
        if (!pointer)
          throw new HevcPlayerError("WASM_MEMORY", "WASM memory allocation failed.");
        try {
          module.HEAPU8.set(data, pointer);
          const result = module._hevc_decoder_push(this.context, pointer, data.byteLength, timestamp);
          if (result !== 0) {
            throw new HevcPlayerError("WASM_DECODE", `libde265 returned error ${result}.`);
          }
          this.#pump();
        } finally {
          module._free(pointer);
        }
      }
      #drainFrames() {
        const module = this.module;
        if (!module)
          return 0;
        let count = 0;
        while (module._hevc_decoder_next_frame(this.context)) {
          try {
            const width = module._hevc_frame_width(this.context);
            const height = module._hevc_frame_height(this.context);
            const yStride = module._hevc_frame_stride(this.context, 0);
            const uStride = module._hevc_frame_stride(this.context, 1);
            const vStride = module._hevc_frame_stride(this.context, 2);
            const yWidth = module._hevc_frame_plane_width(this.context, 0);
            const uWidth = module._hevc_frame_plane_width(this.context, 1);
            const vWidth = module._hevc_frame_plane_width(this.context, 2);
            const yHeight = module._hevc_frame_plane_height(this.context, 0);
            const uHeight = module._hevc_frame_plane_height(this.context, 1);
            const vHeight = module._hevc_frame_plane_height(this.context, 2);
            const yBitDepth = module._hevc_frame_bit_depth(this.context, 0);
            const uBitDepth = module._hevc_frame_bit_depth(this.context, 1);
            const vBitDepth = module._hevc_frame_bit_depth(this.context, 2);
            const fullRange = module._hevc_frame_full_range?.(this.context) === 1;
            const matrixCoefficients = module._hevc_frame_matrix_coefficients?.(this.context) ?? 2;
            const yPtr = module._hevc_frame_plane(this.context, 0);
            const uPtr = module._hevc_frame_plane(this.context, 1);
            const vPtr = module._hevc_frame_plane(this.context, 2);
            const view = (ptr, size) => module.HEAPU8.subarray(ptr, ptr + size);
            const frameInit = {
              timestamp: module._hevc_frame_timestamp(this.context),
              codedWidth: width,
              codedHeight: height,
              displayWidth: width,
              displayHeight: height,
              backend: "wasm"
            };
            const planeMetadata = {
              yStride,
              uStride,
              vStride,
              yWidth,
              uWidth,
              vWidth,
              yHeight,
              uHeight,
              vHeight,
              yBitDepth,
              uBitDepth,
              vBitDepth,
              fullRange,
              matrixCoefficients
            };
            const transientFrame = new UnifiedVideoFrame({
              ...frameInit,
              planes: {
                y: view(yPtr, yStride * yHeight),
                u: view(uPtr, uStride * uHeight),
                v: view(vPtr, vStride * vHeight),
                ...planeMetadata
              }
            });
            try {
              this.hooks.onFrameView?.(transientFrame);
              if (this.hooks.shouldCopyFrames?.() === false) {
                this.options.onFrame(new UnifiedVideoFrame(frameInit));
              } else {
                const planes = transientFrame._planes;
                this.options.onFrame(new UnifiedVideoFrame({
                  ...frameInit,
                  planes: {
                    y: planes.y.slice(),
                    u: planes.u.slice(),
                    v: planes.v.slice(),
                    ...planeMetadata
                  }
                }));
              }
            } finally {
              transientFrame.close();
            }
            count += 1;
          } finally {
            module._hevc_decoder_release_frame(this.context);
          }
        }
        return count;
      }
      #pump() {
        const module = this.module;
        if (!module)
          return;
        this.#drainFrames();
        for (let iterations = 0; iterations < 1e4; iterations += 1) {
          const result = module._hevc_decoder_decode(this.context);
          this.#drainFrames();
          if (result === 13)
            return;
          if (result !== 0 && result !== 9) {
            throw new HevcPlayerError("WASM_DECODE", `libde265 returned error ${result}.`);
          }
          if (!module._hevc_decoder_has_more(this.context) && result !== 9)
            return;
        }
        throw new HevcPlayerError("WASM_DECODE_LOOP", "libde265 decode loop did not converge.");
      }
      async flush() {
        if (!this.module || !this.context)
          return;
        const result = this.module._hevc_decoder_flush(this.context);
        if (result !== 0) {
          throw new HevcPlayerError("WASM_FLUSH", `libde265 returned error ${result}.`);
        }
        this.#pump();
      }
      reset() {
        if (this.module && this.context)
          this.module._hevc_decoder_reset(this.context);
      }
      close() {
        this.closed = true;
        if (this.module && this.context)
          this.module._hevc_decoder_destroy(this.context);
        this.context = 0;
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/backends/wasm-worker.js
var WasmWorkerBackend;
var init_wasm_worker = __esm({
  "scratch/wasm_test/package/dist/esm/backends/wasm-worker.js"() {
    init_errors();
    init_frame();
    WasmWorkerBackend = class _WasmWorkerBackend {
      execution = "worker";
      options;
      handlesRendering = false;
      canvasTransferred = false;
      rendererKind = null;
      worker = null;
      pending = /* @__PURE__ */ new Map();
      nextRequestId = 1;
      queueSize = 0;
      closed = false;
      initialized = false;
      constructor(options) {
        this.options = options;
      }
      static isSupported() {
        return typeof globalThis.Worker === "function";
      }
      get decodeQueueSize() {
        return this.queueSize;
      }
      async init() {
        if (!_WasmWorkerBackend.isSupported()) {
          throw new HevcPlayerError("WORKER_UNSUPPORTED", "Dedicated Worker is not available.");
        }
        const workerUrl = this.options.workerUrl ? new URL(this.options.workerUrl, globalThis.document?.baseURI ?? import.meta.url) : new URL("../workers/wasm-worker.js", import.meta.url);
        const worker = new globalThis.Worker(workerUrl, {
          type: "module",
          name: "libde265-decoder"
        });
        this.worker = worker;
        worker.addEventListener("message", this.#onMessage);
        worker.addEventListener("error", this.#onWorkerError);
        worker.addEventListener("messageerror", this.#onMessageError);
        await this.#request({
          type: "init",
          options: {
            codec: this.options.codec,
            frameDuration: this.options.frameDuration,
            renderer: this.options.renderer,
            frameOutput: this.options.workerFrameOutput,
            ...this.options.wasmBaseUrl === void 0 ? {} : { wasmBaseUrl: new URL(this.options.wasmBaseUrl, globalThis.document?.baseURI ?? import.meta.url).href },
            ...this.options.wasmVariant === void 0 ? {} : { wasmVariant: this.options.wasmVariant },
            ...this.options.threads === void 0 ? {} : { threads: this.options.threads }
          }
        });
        if (this.options.canvas && this.options.render) {
          const offscreen = this.#takeOffscreenCanvas(this.options.canvas);
          if (offscreen) {
            this.canvasTransferred = true;
            const response = await this.#request({
              type: "attach-canvas",
              canvas: offscreen,
              renderer: this.options.renderer
            }, [offscreen]);
            this.handlesRendering = true;
            this.rendererKind = response.rendererKind ?? null;
          }
        }
        this.initialized = true;
      }
      decode(accessUnit, timestamp, duration) {
        if (!this.worker || this.closed) {
          throw new HevcPlayerError("DECODER_CLOSED", "WASM worker is not open.");
        }
        const bytes = accessUnit.data.slice();
        this.queueSize += 1;
        const message = {
          type: "decode",
          data: bytes.buffer,
          key: accessUnit.key,
          timestamp,
          ...duration === void 0 ? {} : { duration }
        };
        this.worker.postMessage(message, [bytes.buffer]);
      }
      async flush() {
        if (!this.worker || this.closed)
          return;
        await this.#request({ type: "flush" });
      }
      reset() {
        if (!this.worker || this.closed)
          return;
        this.worker.postMessage({ type: "reset" });
      }
      close() {
        if (this.closed)
          return;
        this.closed = true;
        if (this.worker) {
          this.worker.postMessage({ type: "close" });
          this.worker.removeEventListener("message", this.#onMessage);
          this.worker.removeEventListener("error", this.#onWorkerError);
          this.worker.removeEventListener("messageerror", this.#onMessageError);
          this.worker.terminate();
        }
        this.worker = null;
        this.queueSize = 0;
        this.#rejectPending(new HevcPlayerError("WORKER_CLOSED", "WASM worker was closed."));
      }
      #takeOffscreenCanvas(canvas) {
        if (typeof globalThis.OffscreenCanvas === "function" && canvas instanceof globalThis.OffscreenCanvas) {
          return canvas;
        }
        const transferable = canvas;
        try {
          return typeof transferable.transferControlToOffscreen === "function" ? transferable.transferControlToOffscreen() : null;
        } catch {
          return null;
        }
      }
      #request(message, transfer = []) {
        if (!this.worker || this.closed) {
          return Promise.reject(new HevcPlayerError("WORKER_CLOSED", "WASM worker is not available."));
        }
        const id = this.nextRequestId++;
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            this.pending.delete(id);
            reject(new HevcPlayerError("WORKER_TIMEOUT", `WASM worker request "${message.type}" timed out.`));
          }, 3e4);
          this.pending.set(id, { resolve, reject, timer });
          this.worker.postMessage({ ...message, id }, transfer);
        });
      }
      #onMessage = (event) => {
        const message = event.data;
        if (message.type === "response") {
          const pending = this.pending.get(message.id);
          if (!pending)
            return;
          this.pending.delete(message.id);
          clearTimeout(pending.timer);
          if (message.ok)
            pending.resolve(message);
          else
            pending.reject(this.#toError(message.error));
          return;
        }
        if (message.type === "dequeue") {
          this.queueSize = Math.max(0, this.queueSize - 1);
          this.options.onDequeue();
          return;
        }
        if (message.type === "error") {
          this.options.onError(this.#toError(message.error));
          return;
        }
        const data = message.frame;
        this.options.onFrame(new UnifiedVideoFrame({
          timestamp: data.timestamp,
          ...data.duration === void 0 ? {} : { duration: data.duration },
          codedWidth: data.codedWidth,
          codedHeight: data.codedHeight,
          displayWidth: data.displayWidth,
          displayHeight: data.displayHeight,
          backend: "wasm",
          ...data.planes ? {
            planes: {
              y: new Uint8Array(data.planes.y.data),
              u: new Uint8Array(data.planes.u.data),
              v: new Uint8Array(data.planes.v.data),
              yStride: data.planes.y.stride,
              uStride: data.planes.u.stride,
              vStride: data.planes.v.stride,
              yWidth: data.planes.y.width,
              uWidth: data.planes.u.width,
              vWidth: data.planes.v.width,
              yHeight: data.planes.y.height,
              uHeight: data.planes.u.height,
              vHeight: data.planes.v.height,
              yBitDepth: data.planes.y.bitDepth,
              uBitDepth: data.planes.u.bitDepth,
              vBitDepth: data.planes.v.bitDepth,
              fullRange: data.planes.fullRange,
              matrixCoefficients: data.planes.matrixCoefficients
            }
          } : {}
        }));
      };
      #onWorkerError = (event) => {
        const error = new HevcPlayerError("WORKER_RUNTIME", event.message || "WASM worker failed.", event.error);
        this.#rejectPending(error);
        if (this.initialized)
          this.options.onError(error);
      };
      #onMessageError = () => {
        const error = new HevcPlayerError("WORKER_MESSAGE", "WASM worker received an uncloneable message.");
        this.#rejectPending(error);
        if (this.initialized)
          this.options.onError(error);
      };
      #rejectPending(error) {
        for (const pending of this.pending.values()) {
          clearTimeout(pending.timer);
          pending.reject(error);
        }
        this.pending.clear();
      }
      #toError(error) {
        return new HevcPlayerError(error.code, error.message);
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/player.js
var player_exports = {};
__export(player_exports, {
  HevcPlayer: () => HevcPlayer
});
function now() {
  return globalThis.performance?.now() ?? Date.now();
}
function normalizeLowLatency(value) {
  const supplied = typeof value === "object" ? value : {};
  const normalized = {
    ...DEFAULT_LOW_LATENCY,
    ...supplied,
    enabled: typeof value === "boolean" ? value : supplied.enabled ?? DEFAULT_LOW_LATENCY.enabled
  };
  for (const name of [
    "maxInputFrames",
    "maxInputBytes",
    "maxDecodeQueue",
    "maxQueueAgeMs"
  ]) {
    if (!Number.isFinite(normalized[name]) || normalized[name] <= 0) {
      throw new HevcPlayerError("INVALID_LOW_LATENCY", `${name} must be a positive finite number.`);
    }
  }
  normalized.maxInputFrames = Math.floor(normalized.maxInputFrames);
  normalized.maxInputBytes = Math.floor(normalized.maxInputBytes);
  normalized.maxDecodeQueue = Math.floor(normalized.maxDecodeQueue);
  return normalized;
}
var MODES, WORKER_MODES, RENDERER_MODES, WORKER_FRAME_OUTPUTS, DEFAULT_LOW_LATENCY, HevcPlayer;
var init_player = __esm({
  "scratch/wasm_test/package/dist/esm/player.js"() {
    init_annexb();
    init_renderer();
    init_events();
    init_errors();
    init_webcodecs();
    init_wasm();
    init_wasm_worker();
    MODES = /* @__PURE__ */ new Set(["auto", "webcodecs", "wasm"]);
    WORKER_MODES = /* @__PURE__ */ new Set(["auto", "disabled", "required"]);
    RENDERER_MODES = /* @__PURE__ */ new Set(["auto", "canvas2d", "webgl"]);
    WORKER_FRAME_OUTPUTS = /* @__PURE__ */ new Set(["planes", "metadata"]);
    DEFAULT_LOW_LATENCY = {
      enabled: true,
      maxInputFrames: 60,
      maxInputBytes: 16 * 1024 * 1024,
      maxDecodeQueue: 8,
      maxQueueAgeMs: 1200,
      dropPolicy: "until-key"
    };
    HevcPlayer = class {
      options;
      events = new EventHub();
      parser = new AnnexBAccessUnitParser();
      renderer = null;
      rendererKind = null;
      wasmExecution = null;
      backend = null;
      state = "idle";
      decoder = null;
      socket = null;
      socketMessageChain = Promise.resolve();
      nextTimestamp = 0;
      inputQueue = [];
      inputQueueBytes = 0;
      drainScheduled = false;
      drainTimer = null;
      inputIdleWaiters = [];
      waitingForKeyframe = false;
      pressureLevel = "normal";
      pendingFrames = [];
      rendering = false;
      renderPromise = Promise.resolve();
      decodedFrames = 0;
      renderedFrames = 0;
      droppedInputFrames = 0;
      droppedRenderFrames = 0;
      constructor(options) {
        if (!options || !MODES.has(options.mode ?? "auto")) {
          throw new HevcPlayerError("INVALID_MODE", "mode must be auto, webcodecs, or wasm.");
        }
        if (!WORKER_MODES.has(options.wasmWorker ?? "auto")) {
          throw new HevcPlayerError("INVALID_WORKER_MODE", "wasmWorker must be auto, disabled, or required.");
        }
        if (!RENDERER_MODES.has(options.renderer ?? "auto")) {
          throw new HevcPlayerError("INVALID_RENDERER", "renderer must be auto, canvas2d, or webgl.");
        }
        if (!WORKER_FRAME_OUTPUTS.has(options.workerFrameOutput ?? "planes")) {
          throw new HevcPlayerError("INVALID_FRAME_OUTPUT", "workerFrameOutput must be planes or metadata.");
        }
        this.options = {
          mode: "auto",
          codec: "hev1.1.6.L93.B0",
          frameDuration: 33333,
          render: true,
          wasmWorker: "auto",
          renderer: "auto",
          workerFrameOutput: "planes",
          ...options,
          lowLatency: normalizeLowLatency(options.lowLatency)
        };
      }
      on(type, listener) {
        return this.events.on(type, listener);
      }
      async init() {
        if (this.state !== "idle")
          return this;
        this.state = "initializing";
        const backendOptions = {
          ...this.options,
          onFrame: (frame) => this.#handleFrame(frame),
          onError: (error) => this.#handleError(error),
          onDequeue: () => this.#scheduleDrain()
        };
        try {
          if (this.options.mode !== "wasm") {
            try {
              const webcodecs = new WebCodecsBackend(backendOptions);
              await webcodecs.init();
              this.decoder = webcodecs;
              this.backend = "webcodecs";
            } catch (error) {
              if (this.options.mode === "webcodecs")
                throw error;
              this.events.emit("fallback", { from: "webcodecs", to: "wasm", error });
            }
          }
          if (!this.decoder) {
            if (this.options.wasmWorker !== "disabled") {
              const worker = new WasmWorkerBackend(backendOptions);
              try {
                await worker.init();
                this.decoder = worker;
                this.wasmExecution = "worker";
              } catch (error) {
                if (worker.canvasTransferred || this.options.wasmWorker === "required") {
                  worker.close();
                  throw error;
                }
                worker.close();
                this.events.emit("workerfallback", {
                  from: "worker",
                  to: "main",
                  error
                });
              }
            }
            if (!this.decoder) {
              const wasm = new WasmBackend(backendOptions);
              await wasm.init();
              this.decoder = wasm;
              this.wasmExecution = "main";
            }
            this.backend = "wasm";
          }
          if (this.options.canvas && this.options.render) {
            if (this.decoder.handlesRendering) {
              this.rendererKind = this.decoder.rendererKind ?? null;
            } else {
              this.renderer = this.backend === "webcodecs" ? new CanvasRenderer(this.options.canvas) : createRenderer(this.options.canvas, this.options.renderer);
              this.rendererKind = this.renderer.kind;
            }
          }
          this.state = "ready";
          this.events.emit("ready", { backend: this.backend });
          return this;
        } catch (error) {
          this.state = "error";
          this.#handleError(error);
          throw error;
        }
      }
      async connect(url = this.options.url, protocols = this.options.protocols) {
        await this.init();
        if (!url)
          throw new HevcPlayerError("WEBSOCKET_URL", "A WebSocket URL is required.");
        this.disconnect();
        const socket = new WebSocket(url, protocols);
        socket.binaryType = "arraybuffer";
        this.socket = socket;
        socket.addEventListener("open", () => this.events.emit("open", { url }));
        socket.addEventListener("message", (event) => {
          this.#queueSocketMessage(event.data);
        });
        socket.addEventListener("close", (event) => {
          this.events.emit("close", { code: event.code, reason: event.reason });
        });
        socket.addEventListener("error", () => {
          this.#handleError(new HevcPlayerError("WEBSOCKET", `WebSocket error for ${url}.`));
        });
        return new Promise((resolve, reject) => {
          socket.addEventListener("open", () => resolve(this), { once: true });
          socket.addEventListener("error", () => reject(new HevcPlayerError("WEBSOCKET_CONNECT", `Could not connect to ${url}.`)), { once: true });
        });
      }
      attach(socket) {
        if (!(socket instanceof WebSocket)) {
          throw new HevcPlayerError("WEBSOCKET_INSTANCE", "attach() expects a WebSocket.");
        }
        this.socket = socket;
        socket.binaryType = "arraybuffer";
        socket.addEventListener("message", (event) => {
          this.#queueSocketMessage(event.data);
        });
        return this;
      }
      push(data, metadata = {}) {
        if (this.state !== "ready") {
          throw new HevcPlayerError("NOT_READY", "Call and await init() before push().");
        }
        const accessUnits = this.parser.push(data);
        let timestamp = metadata.timestamp;
        const duration = metadata.duration ?? this.options.frameDuration;
        for (const accessUnit of accessUnits) {
          this.#enqueue(accessUnit, timestamp === void 0 ? metadata : { ...metadata, timestamp });
          if (timestamp !== void 0)
            timestamp += duration;
        }
        return accessUnits.length;
      }
      pushAccessUnit(data, metadata = {}) {
        if (this.state !== "ready") {
          throw new HevcPlayerError("NOT_READY", "Call and await init() before pushAccessUnit().");
        }
        const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        if (!bytes.byteLength) {
          throw new HevcPlayerError("EMPTY_ACCESS_UNIT", "Access Unit cannot be empty.");
        }
        this.#enqueue({
          data: bytes,
          key: metadata.key ?? isHevcKeyAccessUnit(bytes)
        }, metadata);
      }
      async flush() {
        if (!this.decoder)
          return;
        await this.socketMessageChain;
        for (const accessUnit of this.parser.flush())
          this.#enqueue(accessUnit, {});
        await this.#waitForInputIdle();
        await this.decoder.flush();
        await this.renderPromise;
      }
      reset() {
        this.parser.reset();
        this.decoder?.reset();
        this.#clearInputQueue("waiting-key", false);
        this.waitingForKeyframe = false;
        this.nextTimestamp = 0;
        this.#updatePressure();
      }
      disconnect(code = 1e3, reason = "client disconnect") {
        if (this.socket && this.socket.readyState < WebSocket.CLOSING) {
          this.socket.close(code, reason);
        }
        this.socket = null;
      }
      close() {
        if (this.state === "closed")
          return;
        this.disconnect();
        if (this.drainTimer !== null)
          clearTimeout(this.drainTimer);
        this.drainTimer = null;
        this.#clearInputQueue("waiting-key", false);
        for (const frame of this.pendingFrames.splice(0))
          frame.close();
        this.decoder?.close();
        this.decoder = null;
        this.renderer?.close();
        this.renderer = null;
        this.state = "closed";
        this.events.clear();
      }
      getStats() {
        return {
          backend: this.backend,
          renderer: this.rendererKind,
          wasmExecution: this.wasmExecution,
          inputQueueFrames: this.inputQueue.length,
          inputQueueBytes: this.inputQueueBytes,
          decodeQueueSize: this.decoder?.decodeQueueSize ?? 0,
          queueAgeMs: this.inputQueue.length ? Math.max(0, now() - this.inputQueue[0].queuedAt) : 0,
          waitingForKeyframe: this.waitingForKeyframe,
          decodedFrames: this.decodedFrames,
          renderedFrames: this.renderedFrames,
          droppedInputFrames: this.droppedInputFrames,
          droppedRenderFrames: this.droppedRenderFrames
        };
      }
      #enqueue(accessUnit, metadata) {
        if (!this.decoder) {
          throw new HevcPlayerError("NOT_READY", "Decoder is not initialized.");
        }
        const timestamp = metadata.timestamp ?? this.nextTimestamp;
        const duration = metadata.duration ?? this.options.frameDuration;
        this.nextTimestamp = timestamp + duration;
        const queued = {
          accessUnit,
          timestamp,
          duration,
          queuedAt: now(),
          ...metadata.sequence === void 0 ? {} : { sequence: metadata.sequence }
        };
        if (this.waitingForKeyframe) {
          if (!accessUnit.key) {
            this.#dropInput(queued, "waiting-key");
            return;
          }
          this.waitingForKeyframe = false;
          this.decoder.reset();
        }
        this.inputQueue.push(queued);
        this.inputQueueBytes += accessUnit.data.byteLength;
        const oldestAge = now() - this.inputQueue[0].queuedAt;
        const overflow = this.inputQueue.length > this.options.lowLatency.maxInputFrames || this.inputQueueBytes > this.options.lowLatency.maxInputBytes;
        const stale = oldestAge > this.options.lowLatency.maxQueueAgeMs;
        if (this.options.lowLatency.enabled && this.options.lowLatency.dropPolicy === "until-key" && (overflow || stale)) {
          this.#recoverToKeyframe(overflow ? "input-overflow" : "queue-age", queued);
          return;
        }
        this.#emitQueueChange();
        this.#scheduleDrain();
      }
      #queueSocketMessage(data) {
        this.socketMessageChain = this.socketMessageChain.then(async () => {
          if (data instanceof ArrayBuffer) {
            this.push(new Uint8Array(data));
            return;
          }
          if (data instanceof Blob) {
            this.push(new Uint8Array(await data.arrayBuffer()));
            return;
          }
          throw new HevcPlayerError("WEBSOCKET_MESSAGE", "Expected binary WebSocket messages containing an Annex-B HEVC stream.");
        }).catch((error) => this.#handleError(error));
      }
      #scheduleDrain() {
        if (this.drainScheduled || !this.inputQueue.length || this.state === "closed")
          return;
        this.drainScheduled = true;
        queueMicrotask(() => {
          this.drainScheduled = false;
          this.#drainInput();
        });
      }
      #drainInput() {
        const decoder = this.decoder;
        if (!decoder || this.state === "closed")
          return;
        const started = now();
        while (this.inputQueue.length) {
          if (decoder.decodeQueueSize >= this.options.lowLatency.maxDecodeQueue) {
            this.#updatePressure();
            return;
          }
          const queued = this.inputQueue.shift();
          this.inputQueueBytes -= queued.accessUnit.data.byteLength;
          try {
            decoder.decode(queued.accessUnit, queued.timestamp, queued.duration);
          } catch (error) {
            this.#handleError(error);
            if (this.options.lowLatency.dropPolicy === "until-key") {
              this.#recoverToKeyframe("input-overflow");
              return;
            }
          }
          if (this.backend === "wasm" && now() - started >= 12 && this.inputQueue.length) {
            this.drainTimer = setTimeout(() => {
              this.drainTimer = null;
              this.#scheduleDrain();
            }, 0);
            this.#emitQueueChange();
            return;
          }
        }
        this.#emitQueueChange();
        this.#resolveInputIdle();
      }
      #recoverToKeyframe(reason, newest) {
        const keepNewestKey = newest?.accessUnit.key === true;
        this.decoder?.reset();
        if (keepNewestKey && newest && this.inputQueue.at(-1) === newest) {
          this.inputQueue.pop();
          this.inputQueueBytes -= newest.accessUnit.data.byteLength;
        }
        this.#clearInputQueue(reason, true);
        if (keepNewestKey && newest) {
          this.waitingForKeyframe = false;
          this.inputQueue.push(newest);
          this.inputQueueBytes = newest.accessUnit.data.byteLength;
          this.#emitQueueChange();
          this.#scheduleDrain();
          return;
        }
        this.waitingForKeyframe = true;
        this.events.emit("keyframerequest", { reason });
        this.#emitQueueChange();
      }
      #clearInputQueue(reason, emitDrops) {
        const queued = this.inputQueue;
        this.inputQueue = [];
        this.inputQueueBytes = 0;
        if (emitDrops) {
          for (const item of queued)
            this.#dropInput(item, reason);
        }
        this.#resolveInputIdle();
      }
      #dropInput(item, reason) {
        this.droppedInputFrames += 1;
        this.events.emit("drop", {
          stage: "input",
          reason,
          timestamp: item.timestamp,
          key: item.accessUnit.key,
          ...item.sequence === void 0 ? {} : { sequence: item.sequence }
        });
      }
      #waitForInputIdle() {
        if (!this.inputQueue.length)
          return Promise.resolve();
        return new Promise((resolve) => this.inputIdleWaiters.push(resolve));
      }
      #resolveInputIdle() {
        if (this.inputQueue.length)
          return;
        for (const resolve of this.inputIdleWaiters.splice(0))
          resolve();
      }
      #emitQueueChange() {
        const stats = this.getStats();
        this.events.emit("queuechange", stats);
        this.#updatePressure(stats);
      }
      #updatePressure(stats = this.getStats()) {
        const high = stats.waitingForKeyframe || stats.inputQueueFrames >= Math.ceil(this.options.lowLatency.maxInputFrames * 0.75) || stats.inputQueueBytes >= this.options.lowLatency.maxInputBytes * 0.75 || stats.decodeQueueSize >= this.options.lowLatency.maxDecodeQueue;
        const level = high ? "high" : "normal";
        if (level === this.pressureLevel)
          return;
        this.pressureLevel = level;
        this.events.emit("backpressure", { level, stats });
      }
      #handleFrame(frame) {
        this.decodedFrames += 1;
        if (this.options.lowLatency.enabled && this.rendering && this.pendingFrames.length) {
          const dropped = this.pendingFrames.pop();
          this.droppedRenderFrames += 1;
          this.events.emit("drop", {
            stage: "render",
            reason: "render-backpressure",
            timestamp: dropped.timestamp
          });
          dropped.close();
        }
        this.pendingFrames.push(frame);
        if (!this.rendering) {
          this.renderPromise = this.#drainRender();
        }
      }
      async #drainRender() {
        this.rendering = true;
        try {
          while (this.pendingFrames.length) {
            const frame = this.pendingFrames.shift();
            try {
              await this.options.onFrame?.(frame);
              this.renderer?.render(frame);
              this.events.emit("frame", frame);
              this.renderedFrames += 1;
            } catch (error) {
              this.#handleError(error);
            } finally {
              if (this.options.autoCloseFrames !== false)
                frame.close();
            }
          }
        } finally {
          this.rendering = false;
        }
      }
      #handleError(error) {
        const normalized = error instanceof HevcPlayerError ? error : new HevcPlayerError("UNKNOWN", error instanceof Error ? error.message : String(error), error);
        this.events.emit("error", normalized);
        this.options.onError?.(normalized);
      }
    };
  }
});

// scratch/wasm_test/package/dist/esm/index.js
init_player();
init_annexb();
init_frame();
init_renderer();
init_errors();
async function createHevcPlayer(options) {
  const { HevcPlayer: HevcPlayer2 } = await Promise.resolve().then(() => (init_player(), player_exports));
  const player = new HevcPlayer2(options);
  await player.init();
  return player;
}
export {
  AnnexBAccessUnitParser,
  CanvasRenderer,
  HevcPlayer,
  HevcPlayerError,
  UnifiedVideoFrame,
  WebGLRenderer,
  createHevcPlayer,
  createRenderer,
  hevcNalType,
  isHevcKeyAccessUnit
};
