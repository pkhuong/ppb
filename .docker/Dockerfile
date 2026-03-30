FROM framac/frama-c:32.0

USER root
RUN apt update && apt install -y cvc5 cvc4 z3
ENV OPAMROOTISOK=1
RUN eval $(opam env) && why3 config detect
