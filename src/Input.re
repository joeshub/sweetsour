open Common;

/* An input value that can either be a char or an interpolation */
type inputValue =
  | Char char
  | Interpolation interpolation;

type inputStream = LazyStream.t inputValue;

let input = fun (strings: array string) (interpolations: array interpolation): inputStream => {
  /* A buffer holding a single interpolation value, interleaved between the array of strings */
  let bufferedItem = ref None;

  /* Several integers holding the stream's state */
  let stringsSize = Js.Array.length strings;
  let stringIndex = ref 0;
  let charIndex = ref 0;

  /* Advances the stream's state */
  let advance () => {
    let str = strings.(!stringIndex);
    let strSize = String.length str;

    if (!charIndex >= strSize - 1) {
      /* buffer an interpolation and point to the next string */
      bufferedItem := Some (Interpolation interpolations.(!stringIndex));
      stringIndex := !stringIndex + 1;
      charIndex := 0;
    } else {
      /* jump to next char */
      charIndex := !charIndex + 1;
    }
  };

  /* next function needs to be defined as uncurried and arity-0 at its definition */
  let next: (unit => option inputValue) [@bs] = (fun () => {
    switch !bufferedItem {
      /* emit buffered interpolation before scanning chars */
      | Some item => {
        Some item
      }

      /* get next char and advance, except if all input strings have been emptied */
      | None => {
        if (!stringIndex >= stringsSize) {
          None
        } else {
          let value = strings.(!stringIndex).[!charIndex];
          advance ();
          Some (Char value)
        }
      }
    }
  }) [@bs];

  LazyStream.from next
};