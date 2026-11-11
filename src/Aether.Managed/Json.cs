using System.Globalization;
using System.Text;

namespace Aether {
	/// <summary>A minimal JSON writer (the game has no JSON library the mod can rely on).</summary>
	internal sealed class Json {
		private readonly StringBuilder sb_ = new StringBuilder(1024);
		private bool needComma_;

		public override string ToString() => sb_.ToString();

		private void Comma() {
			if (needComma_) {
				sb_.Append(',');
			}
			needComma_ = true;
		}

		private void Key(string key) {
			Comma();
			if (key != null) {
				String(key);
				sb_.Append(':');
			}
		}

		private void String(string s) {
			sb_.Append('"');
			foreach (char c in s ?? "") {
				switch (c) {
				case '"': sb_.Append("\\\""); break;
				case '\\': sb_.Append("\\\\"); break;
				case '\n': sb_.Append("\\n"); break;
				case '\r': sb_.Append("\\r"); break;
				case '\t': sb_.Append("\\t"); break;
				default:
					if (c < 0x20) {
						sb_.Append("\\u").Append(((int)c).ToString("x4"));
					} else {
						sb_.Append(c);
					}
					break;
				}
			}
			sb_.Append('"');
		}

		// key == null inside an array
		public Json BeginObject(string key = null) { Key(key); sb_.Append('{'); needComma_ = false; return this; }
		public Json EndObject() { sb_.Append('}'); needComma_ = true; return this; }
		public Json BeginArray(string key = null) { Key(key); sb_.Append('['); needComma_ = false; return this; }
		public Json EndArray() { sb_.Append(']'); needComma_ = true; return this; }

		public Json Add(string key, string value) {
			Key(key);
			if (value == null) {
				sb_.Append("null");
			} else {
				String(value);
			}
			return this;
		}

		public Json Add(string key, bool value) { Key(key); sb_.Append(value ? "true" : "false"); return this; }
		public Json Add(string key, int value) { Key(key); sb_.Append(value.ToString(CultureInfo.InvariantCulture)); return this; }
		public Json Add(string key, long value) { Key(key); sb_.Append(value.ToString(CultureInfo.InvariantCulture)); return this; }

		public Json Add(string key, double value) {
			Key(key);
			if (double.IsNaN(value) || double.IsInfinity(value)) {
				sb_.Append("null");
			} else {
				sb_.Append(value.ToString("0.####", CultureInfo.InvariantCulture));
			}
			return this;
		}
	}
}
