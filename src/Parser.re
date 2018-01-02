open Common;

/* an error raised by the parser contains a message and a line number */
exception ParserError(string, locationRange);

/* For RuleStart kinds */
[@bs.deriving jsConverter]
type ruleKind =
  [@bs.as 1] | StyleRule /* CSSOM */
  [@bs.as 2] | CharsetRule /* CSSOM */
  [@bs.as 3] | ImportRule /* CSSOM */
  [@bs.as 4] | MediaRule /* CSSOM */
  [@bs.as 5] | FontFaceRule /* CSSOM */
  [@bs.as 6] | PageRule /* CSSOM */
  [@bs.as 7] | KeyframesRule /* CSS 3 Animations */
  [@bs.as 8] | KeyframeRule /* CSS 3 Animations */
  [@bs.as 9] | MarginRule /* CSSOM */
  [@bs.as 10] | NamespaceRule /* CSSOM */
  [@bs.as 11] | CounterStyleRule /* CSS 3 Lists */
  [@bs.as 12] | SupportsRule /* CSS 3 Conditional */
  [@bs.as 13] | DocumentRule /* CSS 3 Conditional */
  [@bs.as 14] | FontFeatureValuesRule /* CSS 3 Fonts */
  [@bs.as 15] | ViewportRule /* CSS Device Adapt */
  [@bs.as 16] | RegionStyleRule; /* Proposed for CSS 3 Regions */

/* For attribute selector kinds */
[@bs.deriving jsConverter]
type attributeSelectorKind =
  [@bs.as 1] | CaseSensitive
  [@bs.as 2] | CaseInsensitive;

/* A node is represented by its (ISTF) type and potentially a value */
type node =
  | RuleStart(ruleKind)
  | RuleEnd
  | RuleName(string)
  | Selector(string)
  | ParentSelector
  | UniversalSelector
  | CompoundSelectorStart
  | CompoundSelectorEnd
  | SpaceCombinator
  | DoubledChildCombinator
  | ChildCombinator
  | NextSiblingCombinator
  | SubsequentSiblingCombinator
  | Property(string)
  | Value(string)
  | CompoundValueStart
  | CompoundValueEnd
  | Condition(string)
  | FunctionStart(string)
  | FunctionEnd
  | AnimationName(string)
  | SelectorRef(interpolation)
  | PropertyRef(interpolation)
  | ValueRef(interpolation)
  | PartialRef(interpolation)
  | StringStart(string)
  | StringEnd
  | AttributeSelectorStart(attributeSelectorKind)
  | AttributeSelectorEnd
  | AttributeName(string)
  | AttributeOperator(string)
  | AttributeValue(string)
  | AttributeValueRef(interpolation)
  | ConditionRef(interpolation)
  | EOF;

/* Stream type for the ParserStream */
type parserStream = LazyStream.t(node);

/* Modes the parser can be in, allowing encapsulated and specialised logic */
type parserMode =
  | MainLoop
  | PropertyLoop
  | BufferLoop
  | SelectorLoop;

/* compares a last token's end location with the next's start location;
   determines whether they're separated */
let isSeparatedBySpaces = (endLoc: (int, int), startLoc: (int, int)) => {
  let (aRow, aColumn) = endLoc;
  let (bRow, bColumn) = startLoc;
  aRow !== bRow || aColumn + 1 < bColumn
};

/* Running state for parsing */
type state = {
  /* value to keep track of the last token's start and end location */
  mutable tokenRange: locationRange,
  /* value to keep track of the current rule nesting */
  mutable ruleLevel: int,
  /* buffer to hold nodes for the BufferLoop */
  mutable nodeBuffer: LinkedList.t(node),
  /* the current mode of the parser */
  mutable mode: parserMode
};

let parser = (s: Lexer.lexerStream) => {
  let state = {
    tokenRange: { startLoc: (1, 0), endLoc: (1, 0) },
    ruleLevel: 0,
    nodeBuffer: LinkedList.create(),
    mode: MainLoop
  };

  /* a buffer stream that emitts tokens combined with an internal buffer */
  let buffer = BufferStream.from(s);

  /* returns the option(Lexer.token)'s value */
  let getTokenValue = (t: option(Lexer.token)) : option(Lexer.tokenValue) => {
    switch t {
    | Some(Token(value, startLoc, endLoc)) => {
      state.tokenRange = { startLoc, endLoc };
      Some(value)
    }
    | None => None
    }
  };

  /* returns the option(Lexer.token)'s value and range */
  let getTokenValueAndRange = (t: option(Lexer.token)) : option((Lexer.tokenValue, locationRange)) => {
    switch t {
    | Some(Token(value, startLoc, endLoc)) => {
      state.tokenRange = { startLoc, endLoc };
      Some((value, state.tokenRange))
    }
    | None => None
    }
  };

  /* parses a string starting after the first quote */
  let parseString = (kind: Lexer.quoteKind) : LinkedList.t(node) => {
    /* add a Value node to nodeBuffer if the string is not empty */
    let addValueNode = (str: string, nodeBuffer: LinkedList.t(node)) => {
      switch (str) {
      | "" => nodeBuffer
      | _ => {
        LinkedList.add(Value(str), nodeBuffer);
        nodeBuffer
      }
      }
    };

    let nodeBuffer = LinkedList.create();

    /* turn the quote kind into a string */
    let quoteStr = switch (kind) {
      | Double => "\""
      | Single => "'"
    };

    let rec parse = (str: string, containsInterpolation: bool) => {
      switch (getTokenValue(BufferStream.next(buffer))) {
      /* interpolations are added as "ValueRef"s and containsInterpolation is set to true */
      | Some(Interpolation(x)) => {
        /* add the Value of the past str string and then add the ValueRef */
        LinkedList.add(ValueRef(x), addValueNode(str, nodeBuffer));
        parse("", true)
      }

      /* when the ending quote is reached, return the result */
      | Some(Quote(endKind)) when endKind === kind => {
        /* if interpolations or more than one string were parsed, wrap the nodes in a compound */
        if (nodeBuffer.size > 1 || containsInterpolation) {
          LinkedList.unshift(StringStart(quoteStr), addValueNode(str, nodeBuffer));
          LinkedList.add(StringEnd, nodeBuffer);
          nodeBuffer
        } else {
          /* otherwise just add a value node containing the string */
          addValueNode(quoteStr ++ str ++ quoteStr, nodeBuffer)
        }
      }

      /* strings are concatenated into the str argument */
      | Some(Str(rest)) => parse(str ++ rest, containsInterpolation)

      /* all other tokens are invalid inside a string */
      | _ => raise(ParserError(unexpected_msg("token", "string"), state.tokenRange))
      }
    };

    parse("", false)
  };

  /* wraps node buffer in function nodes using the passed function name (fnName) */
  let wrapBufferAsFunction = (nodeBuffer: LinkedList.t(node), fnName: string) => {
    LinkedList.unshift(FunctionStart(fnName), nodeBuffer);
    LinkedList.add(FunctionEnd, nodeBuffer);
    nodeBuffer
  };

  /* parses all selector nodes recursively, including functions and compound selectors,
     and returns the resulting node buffer */
  let parseSelectors = () : LinkedList.t(node) => {
    /* wraps node buffer in compound selector nodes when more than one value was parsed (length > 1) */
    let wrapBufferAsCompound = (nodeBuffer: LinkedList.t(node), length: int) => {
      if (length === 1) {
        nodeBuffer
      } else {
        LinkedList.unshift(CompoundSelectorStart, nodeBuffer);
        LinkedList.add(CompoundSelectorEnd, nodeBuffer);
        nodeBuffer
      }
    };

    /* checks whether the next token forbids a combinator and raises an error if necessary */
    let checkCombinatorValidity = () => {
      switch (getTokenValue(BufferStream.peek(buffer))) {
      | Some(Colon)
      | Some(Comma)
      | Some(Paren(Closing))
      | Some(Brace(Opening)) => {
        raise(ParserError(unexpected_msg("combinator", "selectors"), state.tokenRange));
      }

      /* all other tokens are allowed e.g. interpolation, word... */
      | _ => ()
      }
    };

    /* parse a combinator and fall back to a space combinator;
       also returns the number of nodes that were added */
    let parseCombinator = (nodeBuffer: LinkedList.t(node)) : int => {
      let lastTokenEndLoc = state.tokenRange.endLoc;

      switch (getTokenValueAndRange(BufferStream.peek(buffer))) {
      /* the first tokens here add a combinator and check whether it's permitted before the following token */

      | Some((Arrow, _)) => {
        BufferStream.junk(buffer);

        /* detect whether another arrow follows to switch to a doubled child selector instead */
        switch(getTokenValue(BufferStream.peek(buffer))) {
        | Some(Arrow) => {
          BufferStream.junk(buffer);
          LinkedList.add(DoubledChildCombinator, nodeBuffer);
        }
        | _ => LinkedList.add(ChildCombinator, nodeBuffer);
        };

        checkCombinatorValidity();
        1
      }

      | Some((Plus, _)) => {
        BufferStream.junk(buffer);
        LinkedList.add(NextSiblingCombinator, nodeBuffer);
        checkCombinatorValidity();
        1
      }

      | Some((Tilde, _)) => {
        BufferStream.junk(buffer);
        LinkedList.add(SubsequentSiblingCombinator, nodeBuffer);
        checkCombinatorValidity();
        1
      }

      /* don't add a combinator for these tokens */
      | Some((Comma, _))
      | Some((Paren(_), _))
      | Some((Brace(_), _))
      | None => 0

      /* all other tokens might require a space combinator, e.g. interpolation, word... */
      | Some((_, tokenRange)) => {
        if (isSeparatedBySpaces(lastTokenEndLoc, tokenRange.startLoc)) {
          LinkedList.add(SpaceCombinator, nodeBuffer);
          1
        } else {
          0
        }
      }
      }
    };

    /* recursively parse all selectors by dividing the stream into functions, compounds, and lastly selectors */
    let rec parseSelectorLevel = (nodeBuffer: LinkedList.t(node), length: int, level: int) : LinkedList.t(node) => {
      /* NOTE: This uses BufferStream.peek instead of BufferStream.next, as the final token cannot be put back since the MainLoop uses the LazyStream */
      switch (getTokenValue(BufferStream.next(buffer))) {
      /* parse a pseudo selector or selector function */
      | Some(Colon) => {
        /* check token after colon; this is expected to be a word or an interpolation */
        switch (getTokenValueAndRange(BufferStream.next(buffer))) {
        /* if a function is detected parse it and continue on this level afterwards */
        | Some((Word(word), tokenRange)) => {
          switch (getTokenValue(BufferStream.peek(buffer))) {
          | Some(Paren(Opening)) => {
            BufferStream.junk(buffer);

            /* parse the deeper level and wrap the result in a function */
            let nodes = LinkedList.concat(
              nodeBuffer,
              wrapBufferAsFunction(
                parseSelectorLevel(LinkedList.create(), 0, level + 1),
                ":" ++ word
              )
            );

            /* parse possible combinators */
            let combinatorSize = parseCombinator(nodes);
            /* continue parsing nodes on this level */
            parseSelectorLevel(nodes, length + combinatorSize + 1, level)
          }

          | _ => {
            LinkedList.add(Selector(":" ++ word), nodeBuffer);

            /* set tokenRange to last token (before peek) */
            state.tokenRange = tokenRange;

            /* parse possible combinators */
            let combinatorSize = parseCombinator(nodeBuffer);
            /* parse a combinator and continue parsing nodes on this level */
            parseSelectorLevel(nodeBuffer, length + combinatorSize + 1, level)
          }
          }
        }

        /* parse a pseudo selector with a selector ref */
        | Some((Interpolation(x), _)) => {
          LinkedList.add(Selector(":"), nodeBuffer);
          LinkedList.add(SelectorRef(x), nodeBuffer);

          /* parse possible combinators */
          let combinatorSize = parseCombinator(nodeBuffer);
          /* parse a combinator and continue parsing nodes on this level */
          parseSelectorLevel(nodeBuffer, length + combinatorSize + 2, level)
        }

        /* all other tokens here raise an error, since a pseudo selector must be finished */
        | _ => raise(ParserError(unexpected_msg("token", "pseudo selector"), state.tokenRange))
        }
      }

      /* emit a universal selector and add a combinator */
      | Some(Asterisk) => {
        LinkedList.add(UniversalSelector, nodeBuffer);

        /* parse possible combinators */
        let combinatorSize = parseCombinator(nodeBuffer);
        /* parse a combinator and continue parsing nodes on this level */
        parseSelectorLevel(nodeBuffer, length + combinatorSize + 1, level)
      }

      /* emit a parent selector and add a combinator */
      | Some(Ampersand) => {
        LinkedList.add(ParentSelector, nodeBuffer);

        /* parse possible combinators */
        let combinatorSize = parseCombinator(nodeBuffer);
        /* parse a combinator and continue parsing nodes on this level */
        parseSelectorLevel(nodeBuffer, length + combinatorSize + 1, level)
      }

      /* emit a selector and add a combinator */
      | Some(Word(word)) => {
        LinkedList.add(Selector(word), nodeBuffer);

        /* parse possible combinators */
        let combinatorSize = parseCombinator(nodeBuffer);

        /* parse a combinator and continue parsing nodes on this level */
        parseSelectorLevel(nodeBuffer, length + combinatorSize + 1, level)
      }

      /* emit a selector ref and add a combinator */
      | Some(Interpolation(x)) => {
        LinkedList.add(SelectorRef(x), nodeBuffer);

        /* parse possible combinators */
        let combinatorSize = parseCombinator(nodeBuffer);

        /* parse a combinator and continue parsing nodes on this level */
        parseSelectorLevel(nodeBuffer, length + combinatorSize + 1, level)
      }

      /* wrap the past selectors as compounds, if necessary, and continue parsing on the same level */
      | Some(Comma) => {
        let first = wrapBufferAsCompound(nodeBuffer, length);
        let second = parseSelectorLevel(LinkedList.create(), 0, level);
        /* concatenate the previous and the next nodes */
        LinkedList.concat(first, second)
      }

      /* when the parser is on a deeper level, a closed parenthesis indicates the end of a function */
      | Some(Paren(Closing)) when level > 0 => wrapBufferAsCompound(nodeBuffer, length)

      /* when the parser is not on a deeper level, an opening brace indicates the end of the selectors */
      | Some(Brace(Opening)) when level === 0 => wrapBufferAsCompound(nodeBuffer, length)

      /* EOF or any other tokens are invalid here */
      | _ => raise(ParserError("Unexpected token while parsing selectors", state.tokenRange))
      }
    };

    parseSelectorLevel(LinkedList.create(), 0, 0);
  };

  /* parses all value nodes recursively, including functions and compound values,
     and returns the resulting node buffer */
  let parseValues = () : LinkedList.t(node) => {
    /* wraps node buffer in compound value nodes when more than one value was parsed (length > 1) */
    let wrapBufferAsCompound = (nodeBuffer: LinkedList.t(node), length: int) => {
      if (length === 1) {
        nodeBuffer
      } else {
        LinkedList.unshift(CompoundValueStart, nodeBuffer);
        LinkedList.add(CompoundValueEnd, nodeBuffer);
        nodeBuffer
      }
    };

    /* recursively parse all values by dividing the stream into functions, compounds, and lastly values */
    let rec parseValueLevel = (nodeBuffer: LinkedList.t(node), length: int, level: int) => {
      /* NOTE: This uses BufferStream.peek instead of BufferStream.next, as the final token cannot be put back since the MainLoop uses the LazyStream */
      switch (getTokenValue(BufferStream.peek(buffer))) {
      /* turn words into values or functions */
      | Some(Word(word)) => {
        BufferStream.junk(buffer);

        /* detect opening parentheses to start to parse functions */
        switch (getTokenValue(BufferStream.peek(buffer))) {
        | Some(Paren(Opening)) => {
          BufferStream.junk(buffer);

          /* parse the deeper level and wrap the result in a function */
          let innerValues = wrapBufferAsFunction(
            parseValueLevel(LinkedList.create(), 0, level + 1),
            word
          );

          /* continue parsing nodes on this level */
          parseValueLevel(LinkedList.concat(nodeBuffer, innerValues), length + 1, level)
        }

        /* when the token is a value and not a function, emit the value */
        | _ => {
          LinkedList.add(Value(word), nodeBuffer);
          /* continue parsing the current level */
          parseValueLevel(nodeBuffer, length + 1, level)
        }
        }
      }

      /* detect quotes and start parsing strings */
      | Some(Quote(kind)) => {
        BufferStream.junk(buffer);
        let innerValues = parseString(kind);
        parseValueLevel(LinkedList.concat(nodeBuffer, innerValues), length + 1, level)
      }

      /* free strings belong to url() or calc() and can just be added as values on deeper levels */
      | Some(Str(str)) when level > 0 => {
        BufferStream.junk(buffer);
        LinkedList.add(Value(str), nodeBuffer);
        parseValueLevel(nodeBuffer, length + 1, level)
      }

      /* interpolations are parsed as "ValueRef"s */
      | Some(Interpolation(x)) => {
        BufferStream.junk(buffer);
        LinkedList.add(ValueRef(x), nodeBuffer);
        parseValueLevel(nodeBuffer, length + 1, level)
      }

      /* wrap the past values as compounds, if necessary, and continue parsing on the same level */
      | Some(Comma) => {
        BufferStream.junk(buffer);
        let first = wrapBufferAsCompound(nodeBuffer, length);
        let second = parseValueLevel(LinkedList.create(), 0, level);
        /* concatenate the previous and the next nodes */
        LinkedList.concat(first, second)
      }

      /* when the parser is on a deeper level, a closed parenthesis indicates the end of a function */
      | Some(Paren(Closing)) when level > 0 => {
        BufferStream.junk(buffer);
        wrapBufferAsCompound(nodeBuffer, length)
      }

      /* when the parser is not on a deeper level, the following tokens indicate the end of the values */
      | None
      | Some(Brace(Closing))
      | Some(Semicolon) when level === 0 => {
        wrapBufferAsCompound(nodeBuffer, length)
      }

      /* EOF or any other tokens are invalid here */
      | _ => raise(ParserError(unexpected_msg("token", "values"), state.tokenRange))
      }
    };

    parseValueLevel(LinkedList.create(), 0, 0);
  };

  let propertyLoop = () : node => {
    /* emit node for word or interpolation property */
    let node =
      switch (getTokenValue(BufferStream.next(buffer))) {
      | Some(Word(str)) => Property(str)
      | Some(Interpolation(x)) => PropertyRef(x)
      | _ => raise(ParserError(unexpected_msg("token", "property"), state.tokenRange));
      };

    /* enforce a colon token after a property */
    switch (getTokenValue(BufferStream.next(buffer))) {
    | Some(Colon) => ()
    | _ => raise(ParserError(unexpected_msg("token", "property") ++ expected_msg("colon"), state.tokenRange))
    };

    /* preparse values and start the buffer loop to consume & emit them */
    state.nodeBuffer = parseValues();
    state.mode = BufferLoop;

    node
  };

  /* buffers all tokens until a semicolon or brace is reached to determine whether
     a declaration or selector should be parsed */
  let rec parseDeclOrSelector = () : node => {
    let token = LazyStream.peek(s);

    switch (getTokenValue(token)) {
    /* if an opening curly brace is reached, the selector loop should be triggered;
       it should also be triggered when a token is encountered that the PropertyLoop doesn't support */
    | Some(Brace(Opening))
    | Some(Ampersand)
    | Some(Plus)
    | Some(Arrow)
    | Some(Tilde)
    | Some(Asterisk)
    /* NOTE: The MainLoop skips the initial property's colon so another colon hints at a selector */
    | Some(Colon) => {
      state.ruleLevel = state.ruleLevel + 1;
      state.mode = SelectorLoop;
      RuleStart(StyleRule) /* emit the starting node for the style rule */
    }

    /* if a semicolon or closing curly brace is reached, the property loop should be triggered */
    | Some(Brace(Closing))
    | Some(Semicolon) => {
      state.mode = PropertyLoop;
      propertyLoop()
    }

    /* buffer any other token while skipping it in the LazyStream */
    | Some(_) => {
      BufferStream.bufferOption(token, buffer);
      LazyStream.junk(s);
      parseDeclOrSelector()
    }

    /* raise EOF when the parser is in a partial decl/selector state */
    | None => raise(ParserError(unexpected_msg("eof", "rules") ++ expected_msg("selector, declaration"), state.tokenRange))
    }
  };

  let rec mainLoop = () : node => {
    let firstToken = LazyStream.next(s);
    let secondToken = LazyStream.peek(s);

    switch (getTokenValue(firstToken), getTokenValue(secondToken)) {
    /* skip over free semicolons */
    | (Some(Semicolon), _) => mainLoop()

    /* enter declaration or selector parser */
    | (Some(Word(_)), Some(Colon))
    | (Some(Interpolation(_)), Some(Colon)) => {
      /* buffer first and second token for future decl/selector parsing */
      BufferStream.bufferOption(firstToken, buffer);
      LazyStream.junk(s);
      BufferStream.bufferOption(secondToken, buffer);

      parseDeclOrSelector()
    }

    /* parse at-rules */
    | (Some(AtWord(_)), _) => {
      /* buffer first token for future at-rule parsing */
      BufferStream.bufferOption(firstToken, buffer);
      RuleEnd /* TODO: parse at-rule */
    }

    /* decrease ruleLevel when closing curly brace is encountered */
    | (Some(Brace(Closing)), _) when state.ruleLevel > 0 => {
      state.ruleLevel = state.ruleLevel - 1;
      RuleEnd
    }

    /* fallback to selector parsing; increase ruleLevel and start SelectorLoop */
    | (Some(_), _) => {
      /* buffer first token for future selector parsing */
      BufferStream.bufferOption(firstToken, buffer);

      state.ruleLevel = state.ruleLevel + 1;
      state.mode = SelectorLoop;
      RuleStart(StyleRule) /* emit the starting node for the style rule */
    }

    /* EOF is only allowed when all rules have been closed with closing curly braces */
    | (None, _) when state.ruleLevel > 0 => {
      raise(ParserError(unexpected_msg("eof", "") ++ expected_msg("all rules to be closed"), state.tokenRange));
    }

    /* EOF will be emitted when the ruleLevel === 0 */
    | (None, _) => EOF
    }
  }

  /* emits nodes from a preparsed buffer */
  and bufferLoop = () : node => {
    /* remove a node from the buffered list */
    switch (LinkedList.take(state.nodeBuffer)) {
      /* when the end of the buffered nodes is reached, return to the main loop */
      | None => {
        state.mode = MainLoop;
        mainLoop()
      }

      /* emit node */
      | Some(node) => node
    }
  };

  let selectorLoop = () : node => {
    state.nodeBuffer = parseSelectors();
    state.mode = BufferLoop;
    bufferLoop()
  };

  let next: [@bs] (unit => option(node)) = [@bs] (() => {
    let node =
      switch state.mode {
      | MainLoop => mainLoop()
      | PropertyLoop => propertyLoop()
      | BufferLoop => bufferLoop()
      | SelectorLoop => selectorLoop()
      };

    switch node {
    | EOF => None /* special node to signalise the end of the stream */
    | value => Some(value)
    }
  });

  LazyStream.from(next)
};
