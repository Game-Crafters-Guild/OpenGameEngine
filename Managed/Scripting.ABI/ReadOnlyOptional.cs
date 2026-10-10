using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An optional read-only component value. Used in IEntitySystem.Execute parameters
    /// to access components that may not be present on every entity's archetype.
    /// Zero-allocation: value is copied from the native span, or default if absent.
    /// </summary>
    public readonly struct ReadOnlyOptional<T> where T : unmanaged
    {
        private readonly T _value;
        private readonly bool _hasValue;

        public bool HasValue => _hasValue;

        public T Value
        {
            get
            {
                if (!_hasValue)
                    throw new InvalidOperationException("Optional component is not present");
                return _value;
            }
        }

        public T GetValueOrDefault() => _value;
        public T GetValueOrDefault(T defaultValue) => _hasValue ? _value : defaultValue;

        internal ReadOnlyOptional(T value)
        {
            _value = value;
            _hasValue = true;
        }

        public static ReadOnlyOptional<T> None => default;
    }
}
